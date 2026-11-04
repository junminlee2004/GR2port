// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>

#include "common/logging/log.h"
#include "video_core/gr2_pt/gr2_pt.h"
#include "video_core/gr2_pt/pass_timing.h"
#include "video_core/gr2_pt/shadow_pass.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Gr2Pt::PassTiming {

namespace {

constexpr u32 Passes = ShadowPass::FirstPass + ShadowPass::Cascades;
// Frames in flight in the ring. A frame's results are read Lag frames after its markers, when
// the GPU and the record thread have finished it.
constexpr u32 Slots = 8;
constexpr u32 Lag = Slots / 2;
constexpr u32 Queries = Slots * Passes * 2;
constexpr u32 ReportFrames = 600;

struct Pass {
    u64 cp_start;
    u64 cp_ns;
    u64 game_ns;
    u32 draws;
    bool begun;
    bool ended;
};

struct Slot {
    u32 frame;
    bool used;
    // The frame writes timestamps: its queries were reset.
    bool timed;
    // Timestamps were written and their queries are not reset yet.
    bool pending;
    std::array<Pass, Passes> passes;
};

struct Totals {
    u64 game_ns;
    u64 cp_ns;
    u64 record_ns;
    u64 gpu_ns;
    u64 draws;
    u32 count;
    u32 gpu_count;
};

// Command processor thread only.
struct State {
    bool ready;
    bool gpu;
    vk::Device device;
    vk::QueryPool pool;
    double tick_ns;
    u32 frame;
    u32 frames;
    std::array<Slot, Slots> slots;
    std::array<Totals, Passes> totals;
};
State g_state;

// Written by the closures on the record thread, read Lag frames later.
std::array<std::atomic<u64>, Queries> g_record_clock;

u64 Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

u32 Query(u32 slot, u32 pass, u32 end) {
    return (slot * Passes + pass) * 2 + end;
}

void Init(const Vulkan::Rasterizer& rasterizer) {
    g_state.ready = true;
    const Vulkan::Instance& instance = RasterizerAccess::Instance(rasterizer);
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
    const auto features =
        physical.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan12Features>();
    g_state.tick_ns = physical.getProperties().limits.timestampPeriod;
    g_state.device = instance.GetDevice();
    if (!features.get<vk::PhysicalDeviceVulkan12Features>().hostQueryReset) {
        LOG_WARNING(Render_Vulkan, "GR2 PT: no host query reset, passes are timed on the CPU only");
        return;
    }
    auto [result, pool] = g_state.device.createQueryPool({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = Queries,
    });
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "GR2 PT: no timestamp query pool: {}", vk::to_string(result));
        return;
    }
    g_state.pool = pool;
    g_state.device.resetQueryPool(pool, 0, Queries);
    g_state.gpu = true;
}

void Stamp(Vulkan::Scheduler& scheduler, u32 query, bool gpu) {
    g_record_clock[query].store(0, std::memory_order_relaxed);
    scheduler.Record([pool = g_state.pool, query, gpu](vk::CommandBuffer cmd) {
        if (gpu) {
            cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool, query);
        }
        g_record_clock[query].store(Now(), std::memory_order_relaxed);
    });
}

void Report() {
    Totals frame{};
    for (u32 p = 0; p < Passes; p++) {
        const Totals& t = g_state.totals[p];
        const double n = t.count ? t.count : 1;
        const double g = t.gpu_count ? t.gpu_count : 1;
        LOG_INFO(Render_Vulkan,
                 "GR2 PT timing: pass {} over {} frames: {:.0f} draws, draw thread {:.3f} ms, "
                 "command processor {:.3f} ms, record thread {:.3f} ms, GPU {:.3f} ms",
                 p, t.count, t.draws / n, t.game_ns / n * 1e-6, t.cp_ns / n * 1e-6,
                 t.record_ns / n * 1e-6, t.gpu_ns / g * 1e-6);
        frame.draws += t.count ? t.draws / t.count : 0;
        frame.game_ns += t.count ? t.game_ns / t.count : 0;
        frame.cp_ns += t.count ? t.cp_ns / t.count : 0;
        frame.record_ns += t.count ? t.record_ns / t.count : 0;
        frame.gpu_ns += t.gpu_count ? t.gpu_ns / t.gpu_count : 0;
    }
    LOG_INFO(Render_Vulkan,
             "GR2 PT timing: shadow casters per frame: {} draws, draw thread {:.3f} ms, command "
             "processor {:.3f} ms, record thread {:.3f} ms, GPU {:.3f} ms",
             frame.draws, frame.game_ns * 1e-6, frame.cp_ns * 1e-6, frame.record_ns * 1e-6,
             frame.gpu_ns * 1e-6);
    g_state.totals = {};
}

void Collect(u32 s) {
    Slot& slot = g_state.slots[s];
    std::array<u64, Passes * 4> results{};
    bool available = false;
    if (slot.pending) {
        // Each query's value and availability; a pass that did not run reads unavailable.
        static_cast<void>(g_state.device.getQueryPoolResults(
            g_state.pool, Query(s, 0, 0), Passes * 2, sizeof(results), results.data(),
            sizeof(u64) * 2,
            vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability));
        available = true;
    }
    bool written_ready = true;
    for (u32 p = 0; p < Passes; p++) {
        const Pass& pass = slot.passes[p];
        if (!pass.begun || !pass.ended) {
            continue;
        }
        Totals& t = g_state.totals[p];
        t.game_ns += pass.game_ns;
        t.cp_ns += pass.cp_ns;
        t.draws += pass.draws;
        const u64 rb = g_record_clock[Query(s, p, 0)].load(std::memory_order_relaxed);
        const u64 re = g_record_clock[Query(s, p, 1)].load(std::memory_order_relaxed);
        t.record_ns += rb && re > rb ? re - rb : 0;
        ++t.count;
        if (!available || !slot.timed) {
            continue;
        }
        const u64* begin = &results[p * 4];
        const u64* end = &results[p * 4 + 2];
        if (begin[1] && end[1]) {
            t.gpu_ns += static_cast<u64>((end[0] - begin[0]) * g_state.tick_ns);
            ++t.gpu_count;
        } else {
            written_ready = false;
        }
    }
    if (slot.pending && written_ready) {
        g_state.device.resetQueryPool(g_state.pool, Query(s, 0, 0), Passes * 2);
        slot.pending = false;
    }
    slot.used = false;
    if (++g_state.frames % ReportFrames == 0) {
        Report();
    }
}

void StartFrame(u32 frame) {
    g_state.frame = frame;
    const u32 old = (frame - Lag) % Slots;
    if (g_state.slots[old].used && g_state.slots[old].frame == frame - Lag) {
        Collect(old);
    }
    const u32 s = frame % Slots;
    if (g_state.slots[s].used) {
        Collect(s);
    }
    Slot& slot = g_state.slots[s];
    slot.frame = frame;
    slot.used = true;
    slot.timed = g_state.gpu && !slot.pending;
    slot.pending |= slot.timed;
    slot.passes = {};
}

} // namespace

void OnMarker(Vulkan::Rasterizer& rasterizer, const Marker::Words& words) {
    if (words.pass >= Passes) {
        return;
    }
    if (!g_state.ready) {
        Init(rasterizer);
    }
    if (words.kind == Marker::Kind::PassBegin && words.frame != g_state.frame) {
        StartFrame(words.frame);
    }
    const u32 s = words.frame % Slots;
    Slot& slot = g_state.slots[s];
    if (!slot.used || slot.frame != words.frame) {
        return;
    }
    Pass& pass = slot.passes[words.pass];
    switch (words.kind) {
    case Marker::Kind::PassBegin:
        if (pass.begun) {
            return;
        }
        pass.begun = true;
        pass.cp_start = Now();
        Stamp(rasterizer.GetScheduler(), Query(s, words.pass, 0), slot.timed);
        break;
    case Marker::Kind::PassEnd:
        if (!pass.begun || pass.ended) {
            return;
        }
        pass.ended = true;
        pass.cp_ns = Now() - pass.cp_start;
        pass.game_ns = words.game_ns;
        pass.draws = words.draws;
        Stamp(rasterizer.GetScheduler(), Query(s, words.pass, 1), slot.timed);
        break;
    }
}

} // namespace Gr2Pt::PassTiming
