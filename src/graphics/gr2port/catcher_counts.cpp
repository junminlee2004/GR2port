// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "graphics/gr2port/catcher_counts.h"
#include "graphics/gr2port/catcher_note.h"
#include "graphics/gr2port/memory.h"

namespace Gr2Port::Catcher::Counts {

namespace {

// What shaders/catcher.glsl counts, in its order of ids.
constexpr std::array Names{
    "lanes no turn of the character programs' per-draw loop reached",
    "lanes no turn of the character depth program's per-draw loop reached",
    "lanes no turn of the world programs' per-draw loop reached",
    "emitter steps whose spawning ring past the particle pool was cut",
    "emitter steps whose parents' ring past the particle pool was cut",
    "emitter steps whose parents' spawns stopped at the particle pool's size",
    "emitter steps whose own spawns stopped at the particle pool's size",
};
constexpr u64 Bytes = 64 * sizeof(u32);
static_assert(Names.size() * sizeof(u32) <= Bytes);
constexpr std::chrono::seconds Period{2};

struct Counter {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderStageFlags stages = 0;
};

PFN_vkCmdBindPipeline g_bind = nullptr;
PFN_vkCmdPushConstants g_push = nullptr;
PFN_vkDestroyPipeline g_destroy_pipeline = nullptr;

// The pipelines of catcher builds.
std::mutex g_mutex;
std::unordered_map<VkPipeline, Counter> g_counting;

Buffer g_counts;
// The GPU's 32-bit counts as read last, and the totals since the start: a count moves less than
// 2^32 between two reads.
std::array<u32, Names.size()> g_read{};
std::array<u64, Names.size()> g_totals{};
std::array<u64, Names.size()> g_logged{};
std::chrono::steady_clock::time_point g_logged_at;

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline,
                                           const VkAllocationCallbacks* allocator) {
    {
        const std::scoped_lock lock{g_mutex};
        g_counting.erase(pipeline);
    }
    g_destroy_pipeline(device, pipeline, allocator);
}

VKAPI_ATTR void VKAPI_CALL BindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                        VkPipeline pipeline) {
    g_bind(cmd, point, pipeline);
    Counter counter;
    {
        const std::scoped_lock lock{g_mutex};
        const auto found = g_counting.find(pipeline);
        if (found == g_counting.end()) {
            return;
        }
        counter = found->second;
    }
    // 0 before the counts' buffer exists, which the programs skip.
    const u64 address = g_counts.address;
    g_push(cmd, counter.layout, counter.stages, Offset, sizeof(address), &address);
}

void LogNow() {
    std::array<u32, Names.size()> counts;
    std::memcpy(counts.data(), g_counts.mapped, sizeof(counts));
    for (u32 i = 0; i < counts.size(); i++) {
        g_totals[i] += counts[i] - g_read[i];
    }
    g_read = counts;
    if (g_totals == g_logged) {
        return;
    }
    g_logged = g_totals;
    std::string line;
    for (u32 i = 0; i < g_totals.size(); i++) {
        if (g_totals[i] != 0) {
            line += fmt::format("{}{} {}", line.empty() ? "" : "; ", g_totals[i], Names[i]);
        }
    }
    LOG_WARNING(Render_Vulkan, "GR2port catcher: loop limits reached since the start: {}", line);
}

} // namespace

u32 PushBytes(u32 bytes) {
    return Catcher::On ? Offset + sizeof(u64) : bytes;
}

void Install(Device& device) {
    auto& dl = device.dispatch;
    g_bind = std::exchange(dl.vkCmdBindPipeline, BindPipeline);
    g_push = dl.vkCmdPushConstants;
    g_destroy_pipeline = std::exchange(dl.vkDestroyPipeline, DestroyPipeline);
}

void Start(Memory& memory) {
    g_counts =
        memory.CreateBuffer(Bytes, vk::BufferUsageFlagBits::eStorageBuffer, Placement::Readback);
    if (g_counts.mapped != nullptr) {
        std::memset(g_counts.mapped, 0, Bytes);
    }
    LOG_INFO(Render_Vulkan,
             "GR2port catcher: the catcher's builds of the programs with loop "
             "limits {}",
             g_counts.mapped != nullptr ? "count the limits they reach"
                                        : "count nothing: their buffer failed");
}

void Counting(const Pipeline& pipeline, vk::ShaderStageFlags stages) {
    if (!Catcher::On || !pipeline.pipeline) {
        return;
    }
    const std::scoped_lock lock{g_mutex};
    g_counting[pipeline.pipeline] = {pipeline.layout, static_cast<VkShaderStageFlags>(stages)};
}

void Log() {
    if (g_counts.mapped == nullptr) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_logged_at < Period) {
        return;
    }
    g_logged_at = now;
    LogNow();
}

void Report() {
    if (g_counts.mapped != nullptr) {
        LogNow();
    }
}

} // namespace Gr2Port::Catcher::Counts
