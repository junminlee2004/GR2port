// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/thread.h"
#include "graphics/gr2port/device.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/low_latency.h"

namespace Gr2Port::LowLatency {

namespace {

using Clock = std::chrono::steady_clock;
using Setting = Settings::LowLatency;

// The semaphore the game's main thread waits on for its draw thread to end a frame.
constexpr VAddr DrawSemaphore = 0x1cdee30;

// Universal: the delay leaves the frames this much of their wait for the draw thread, which the
// changes in their own lengths take up. It follows the least of the last SlackFrames frames.
constexpr Clock::duration Margin = std::chrono::microseconds{1500};
constexpr u32 SlackFrames = 8;

// A vblank, or the driver's sleep, that takes longer than this lets the frame start anyway.
constexpr std::chrono::milliseconds LongestWait{100};

const Device* g_device = nullptr;
std::atomic<Setting> g_mode{Setting::Off};

// Reflex's sleep and markers name the swapchain, which the render thread replaces.
std::shared_mutex g_swapchain_mutex;
vk::SwapchainKHR g_swapchain;
vk::Semaphore g_sleep;
u64 g_sleep_value = 0;

// The vblanks that took a flip, which the frames' starts wait for.
std::mutex g_vblank_mutex;
std::condition_variable g_vblank_cv;
u64 g_vblanks = 0;

// The game's main thread's own: whether this is it, whether its frame started, the vblank it
// started at, the frame's number, its wait for the draw thread, Universal's delay and the waits
// the delay could have taken.
thread_local bool t_main = false;
bool g_started = false;
u64 g_started_vblank = 0;
u64 g_frame = 0;
bool g_waiting = false;
Clock::time_point g_wait_start;
Clock::duration g_delay{};
std::array<Clock::duration, SlackFrames> g_slack{};
u32 g_slack_next = 0;

// The frame the main thread hands to the draw thread.
std::atomic<u64> g_handed{0};

// The game's frames the draw thread captured, with their frames' numbers.
struct Captured {
    u64 frame;
    u64 low_latency_frame;
};
std::mutex g_captured_mutex;
std::array<Captured, 16> g_captured{};

// The render thread's last frame.
u64 g_drawn = 0;

// For the report: frame starts, and their waits in nanoseconds for the vblank, for the driver's
// sleep or the delay, and for the draw thread.
std::atomic<u64> g_starts{0};
std::atomic<u64> g_vblank_ns{0};
std::atomic<u64> g_sleep_ns{0};
std::atomic<u64> g_draw_ns{0};

bool On() {
    return g_mode.load(std::memory_order_relaxed) != Setting::Off;
}

u64 Nanoseconds(Clock::duration duration) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

bool IsDrawSemaphore(u32 semaphore) {
    return semaphore == *reinterpret_cast<const u32*>(MemoryPatcher::g_eboot_address +
                                                      DrawSemaphore - Hooks::CodeBase);
}

void Marker(vk::LatencyMarkerNV marker, u64 frame) {
    if (frame == 0) {
        return;
    }
    std::shared_lock lock{g_swapchain_mutex};
    if (g_swapchain) {
        g_device->device.setLatencyMarkerNV(g_swapchain, {.presentID = frame, .marker = marker},
                                            g_device->dispatch);
    }
}

// Reflex's sleep: the driver signals the semaphore when the frame should start.
void ReflexSleep() {
    u64 value = 0;
    {
        std::shared_lock lock{g_swapchain_mutex};
        if (!g_swapchain) {
            return;
        }
        value = ++g_sleep_value;
        const vk::Result slept = g_device->device.latencySleepNV(
            g_swapchain, {.signalSemaphore = g_sleep, .value = value}, g_device->dispatch);
        if (slept != vk::Result::eSuccess) {
            return;
        }
    }
    const vk::SemaphoreWaitInfo wait{
        .semaphoreCount = 1,
        .pSemaphores = &g_sleep,
        .pValues = &value,
    };
    (void)g_device->device.waitSemaphores(
        wait, std::chrono::duration_cast<std::chrono::nanoseconds>(LongestWait).count(),
        g_device->dispatch);
}

void AntiLag(vk::AntiLagStageAMD stage, u64 frame) {
    const vk::AntiLagPresentationInfoAMD presentation{.stage = stage, .frameIndex = frame};
    g_device->device.antiLagUpdateAMD(
        {.mode = vk::AntiLagModeAMD::eOn, .maxFPS = 0, .pPresentationInfo = &presentation},
        g_device->dispatch);
}

// The main thread starts a frame at a vblank that took a flip since the last start, or at once
// when one did, then sleeps as the mode does.
void StartFrame(bool input) {
    g_started = true;
    const Clock::time_point start = Clock::now();
    {
        std::unique_lock lock{g_vblank_mutex};
        g_vblank_cv.wait_for(lock, LongestWait, [] { return g_vblanks != g_started_vblank; });
        g_started_vblank = g_vblanks;
    }
    const Clock::time_point vblank = Clock::now();
    const Setting mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Setting::Universal && g_delay > Clock::duration::zero()) {
        Common::AccurateSleep(std::chrono::duration_cast<std::chrono::nanoseconds>(g_delay),
                              nullptr, false);
    }
    g_frame++;
    if (mode == Setting::Reflex) {
        ReflexSleep();
        Marker(vk::LatencyMarkerNV::eSimulationStart, g_frame);
        if (input) {
            Marker(vk::LatencyMarkerNV::eInputSample, g_frame);
        }
    } else if (mode == Setting::AntiLag2) {
        AntiLag(vk::AntiLagStageAMD::eInput, g_frame);
    }
    g_starts.fetch_add(1, std::memory_order_relaxed);
    g_vblank_ns.fetch_add(Nanoseconds(vblank - start), std::memory_order_relaxed);
    g_sleep_ns.fetch_add(Nanoseconds(Clock::now() - vblank), std::memory_order_relaxed);
}

// Universal: the frame waited for the draw thread; the next starts later by the least of the last
// frames' delays and waits, less the margin, within most of a frame.
void Follow(Clock::duration waited) {
    if (g_mode.load(std::memory_order_relaxed) != Setting::Universal) {
        g_delay = {};
        return;
    }
    g_slack[g_slack_next] = g_delay + waited;
    g_slack_next = (g_slack_next + 1) % SlackFrames;
    const Clock::duration least = *std::ranges::min_element(g_slack);
    const u32 frame_rate = std::max(Settings::Get().game.frame_rate, 1u);
    const Clock::duration longest = std::chrono::duration_cast<Clock::duration>(
        std::chrono::nanoseconds{750'000'000 / frame_rate});
    g_delay = std::clamp(least - Margin, Clock::duration::zero(), longest);
}

const char* Name(Setting mode) {
    switch (mode) {
    case Setting::Reflex:
        return "NVIDIA Reflex";
    case Setting::AntiLag2:
        return "AMD Anti-Lag 2";
    case Setting::Universal:
        return "Universal";
    default:
        return "off";
    }
}

} // namespace

void Start(const Device& device) {
    g_device = &device;
    if (!device.low_latency2) {
        return;
    }
    vk::SemaphoreTypeCreateInfo type{.semaphoreType = vk::SemaphoreType::eTimeline};
    const auto [result, semaphore] =
        device.device.createSemaphore({.pNext = &type}, nullptr, device.dispatch);
    if (result == vk::Result::eSuccess) {
        g_sleep = semaphore;
    }
}

bool Has(Setting mode) {
    if (mode == Setting::Reflex) {
        return g_device != nullptr && g_device->low_latency2 && g_sleep;
    }
    if (mode == Setting::AntiLag2) {
        return g_device != nullptr && g_device->anti_lag;
    }
    return true;
}

bool Choose(Setting wanted) {
    const Setting mode = Has(wanted) ? wanted : Setting::Universal;
    const Setting before = g_mode.exchange(mode, std::memory_order_relaxed);
    if (mode == before) {
        return false;
    }
    if (before == Setting::AntiLag2) {
        const vk::AntiLagDataAMD off{.mode = vk::AntiLagModeAMD::eOff};
        g_device->device.antiLagUpdateAMD(off, g_device->dispatch);
    }
    if (mode != wanted) {
        LOG_INFO(Render_Vulkan, "GR2port: the GPU lacks {}: low latency is {}", Name(wanted),
                 Name(mode));
    } else {
        LOG_INFO(Render_Vulkan, "GR2port: low latency is {}", Name(mode));
    }
    return (before == Setting::Reflex) != (mode == Setting::Reflex);
}

Setting Mode() {
    return g_mode.load(std::memory_order_relaxed);
}

bool FlipsAll() {
    return On();
}

void OnFlipVblank() {
    {
        std::scoped_lock lock{g_vblank_mutex};
        g_vblanks++;
    }
    g_vblank_cv.notify_all();
}

void BeforeInput() {
    if (t_main && !g_started && On()) {
        StartFrame(true);
    }
}

void BeforeWait(u32 semaphore) {
    if (!On() || !IsDrawSemaphore(semaphore)) {
        return;
    }
    if (!t_main) {
        t_main = true;
        LOG_INFO(Render_Vulkan,
                 "GR2port: the game's main thread waits for its draw thread on semaphore {}",
                 semaphore);
    }
    // A frame that read no pad starts here, so that it waits for the vblank all the same.
    if (!g_started) {
        StartFrame(false);
    }
    g_started = false;
    g_waiting = true;
    g_wait_start = Clock::now();
}

void AfterWait(u32 semaphore) {
    if (!t_main || !g_waiting || !IsDrawSemaphore(semaphore)) {
        return;
    }
    g_waiting = false;
    const Clock::duration waited = Clock::now() - g_wait_start;
    g_draw_ns.fetch_add(Nanoseconds(waited), std::memory_order_relaxed);
    g_handed.store(g_frame, std::memory_order_release);
    if (Mode() == Setting::Reflex) {
        Marker(vk::LatencyMarkerNV::eSimulationEnd, g_frame);
    }
    Follow(waited);
}

void Draws(u64 frame) {
    if (!On()) {
        return;
    }
    const u64 handed = g_handed.load(std::memory_order_acquire);
    {
        std::scoped_lock lock{g_captured_mutex};
        g_captured[frame % g_captured.size()] = {frame, handed};
    }
    if (Mode() == Setting::Reflex) {
        Marker(vk::LatencyMarkerNV::eRendersubmitStart, handed);
    }
}

u64 FrameOf(u64 frame) {
    if (!On()) {
        return 0;
    }
    Captured captured;
    {
        std::scoped_lock lock{g_captured_mutex};
        captured = g_captured[frame % g_captured.size()];
    }
    // A present's id must be larger than the last one's.
    if (captured.frame != frame || captured.low_latency_frame <= g_drawn) {
        return 0;
    }
    g_drawn = captured.low_latency_frame;
    return g_drawn;
}

bool ReflexSwapchain() {
    return Mode() == Setting::Reflex;
}

void SwapchainMade(vk::SwapchainKHR swapchain) {
    if (!ReflexSwapchain()) {
        return;
    }
    std::unique_lock lock{g_swapchain_mutex};
    g_swapchain = swapchain;
    const vk::Result result = g_device->device.setLatencySleepModeNV(
        swapchain, {.lowLatencyMode = true, .lowLatencyBoost = false, .minimumIntervalUs = 0},
        g_device->dispatch);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: Reflex's sleep mode failed: {}", vk::to_string(result));
    }
}

void SwapchainGone() {
    std::unique_lock lock{g_swapchain_mutex};
    g_swapchain = nullptr;
}

void Submitted(u64 frame) {
    if (Mode() == Setting::Reflex) {
        Marker(vk::LatencyMarkerNV::eRendersubmitEnd, frame);
    }
}

void BeforePresent(u64 frame) {
    if (frame == 0) {
        return;
    }
    if (Mode() == Setting::Reflex) {
        Marker(vk::LatencyMarkerNV::ePresentStart, frame);
    } else if (Mode() == Setting::AntiLag2) {
        AntiLag(vk::AntiLagStageAMD::ePresent, frame);
    }
}

void AfterPresent(u64 frame) {
    if (Mode() == Setting::Reflex) {
        Marker(vk::LatencyMarkerNV::ePresentEnd, frame);
    }
}

std::string Report(double seconds) {
    const u64 starts = g_starts.exchange(0, std::memory_order_relaxed);
    const double vblank = static_cast<double>(g_vblank_ns.exchange(0, std::memory_order_relaxed));
    const double sleep = static_cast<double>(g_sleep_ns.exchange(0, std::memory_order_relaxed));
    const double draw = static_cast<double>(g_draw_ns.exchange(0, std::memory_order_relaxed));
    if (!On()) {
        return {};
    }
    const double per_frame = 1e6 * static_cast<double>(std::max<u64>(starts, 1));
    return fmt::format("{}, {:.1f} frame starts/s; per frame the vblank {:.2f} ms, {} {:.2f} ms, "
                       "the draw thread {:.2f} ms",
                       Name(Mode()), static_cast<double>(starts) / seconds, vblank / per_frame,
                       Mode() == Setting::Universal ? "the delay" : "the driver's sleep",
                       sleep / per_frame, draw / per_frame);
}

} // namespace Gr2Port::LowLatency
