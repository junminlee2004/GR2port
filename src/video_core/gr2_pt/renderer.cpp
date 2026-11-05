// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/gr2_pt/capture.h"
#include "video_core/gr2_pt/device.h"
#include "video_core/gr2_pt/memory.h"
#include "video_core/gr2_pt/renderer.h"
#include "video_core/gr2_pt/scene_ring.h"
#include "video_core/gr2_pt/swapchain.h"
#include "video_core/gr2_pt/window.h"

namespace Gr2Pt::Renderer {

namespace {

constexpr u32 FramesInFlight = 2;
constexpr u32 ReportFrames = 600;

struct Slot {
    vk::CommandPool pool;
    vk::CommandBuffer commands;
    vk::Fence done;
    vk::Semaphore acquired;
    // The scene ring slot the frame reads.
    u32 scene = SceneRing::Slots;
    bool timed = false;
};

struct Stats {
    u64 game_frames = 0;
    u64 skipped = 0;
    u32 frames = 0;
    u32 timed = 0;
    double cpu_ms = 0.0;
    double gpu_ms = 0.0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

class Thread {
public:
    void Run();

private:
    bool Init();
    void Frame(const SceneRing::Packet& packet);
    void Record(Slot& slot, u32 index, u64 game_frame);
    void ReadTimes(u32 slot_index);
    void Report();
    bool Recreate();

    Device d;
    std::unique_ptr<Memory> memory;
    Buffer scene_ring;
    std::unique_ptr<Swapchain> swapchain;
    std::array<Slot, FramesInFlight> slots;
    vk::QueryPool timestamps;
    u32 next = 0;
    bool needs_swapchain = false;
    Stats stats;
};

void Barrier(const Device& d, vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from,
             vk::ImageLayout to, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         d.dispatch);
}

bool Thread::Init() {
    SDL_Window* const window = Window::Create();
    if (window == nullptr || !CreateDevice(window, d)) {
        return false;
    }
    const auto& dl = d.dispatch;
    for (Slot& slot : slots) {
        slot.pool = d.device
                        .createCommandPool({.flags = vk::CommandPoolCreateFlagBits::eTransient,
                                            .queueFamilyIndex = d.queue_family},
                                           nullptr, dl)
                        .value;
        const vk::CommandBufferAllocateInfo info{
            .commandPool = slot.pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        };
        slot.commands = d.device.allocateCommandBuffers(info, dl).value.front();
        slot.done =
            d.device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}, nullptr, dl).value;
        slot.acquired = d.device.createSemaphore({}, nullptr, dl).value;
    }
    if (d.timestamp_period != 0.0) {
        const vk::QueryPoolCreateInfo info{
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = 2 * FramesInFlight,
        };
        timestamps = d.device.createQueryPool(info, nullptr, dl).value;
    }
    memory = std::make_unique<Memory>(d);
    if (!memory->Create()) {
        return false;
    }
    scene_ring = memory->CreateBuffer(SceneRing::Slots * SceneRing::SlotBytes,
                                      vk::BufferUsageFlagBits::eStorageBuffer, Placement::Upload);
    if (scene_ring.mapped == nullptr) {
        return false;
    }
    SceneRing::Attach(scene_ring.mapped, scene_ring.address);
    swapchain = std::make_unique<Swapchain>(d);
    needs_swapchain = true;
    return true;
}

bool Thread::Recreate() {
    const Window::Size size = Window::PixelSize();
    if (d.device.waitIdle(d.dispatch) != vk::Result::eSuccess) {
        return false;
    }
    needs_swapchain = !swapchain->Create(size.width, size.height);
    return !needs_swapchain;
}

void Thread::ReadTimes(u32 slot_index) {
    Slot& slot = slots[slot_index];
    if (!slot.timed) {
        return;
    }
    slot.timed = false;
    std::array<u64, 2> ticks{};
    const vk::Result result =
        d.device.getQueryPoolResults(timestamps, 2 * slot_index, 2, sizeof(ticks), ticks.data(),
                                     sizeof(u64), vk::QueryResultFlagBits::e64, d.dispatch);
    if (result == vk::Result::eSuccess && ticks[1] >= ticks[0]) {
        stats.gpu_ms += static_cast<double>(ticks[1] - ticks[0]) * d.timestamp_period * 1e-6;
        stats.timed++;
    }
}

void Thread::Record(Slot& slot, u32 index, u64 game_frame) {
    const vk::CommandBuffer cmd = slot.commands;
    const auto& dl = d.dispatch;
    const u32 slot_index = static_cast<u32>(&slot - slots.data());
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    if (timestamps) {
        cmd.resetQueryPool(timestamps, 2 * slot_index, 2, dl);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, timestamps, 2 * slot_index, dl);
    }
    const vk::Image image = swapchain->Image(index);
    Barrier(d, cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);

    // The clear colour turns once every 600 game frames.
    const float phase = static_cast<float>(game_frame % 600) / 600.0f * 6.2831853f;
    const vk::ClearValue clear{
        .color = vk::ClearColorValue{
            .float32 = std::array<float, 4>{0.25f + 0.2f * std::sin(phase),
                                            0.25f + 0.2f * std::sin(phase + 2.1f),
                                            0.25f + 0.2f * std::sin(phase + 4.2f), 1.0f}}};
    const vk::RenderingAttachmentInfo color{
        .imageView = swapchain->View(index),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = clear,
    };
    const vk::RenderingInfo rendering{
        .renderArea = {{0, 0}, swapchain->Extent()},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    cmd.beginRendering(rendering, dl);
    cmd.endRendering(dl);

    Barrier(d, cmd, image, vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::ePresentSrcKHR, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eNone,
            vk::AccessFlagBits2::eNone);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, timestamps,
                            2 * slot_index + 1, dl);
        slot.timed = true;
    }
    (void)cmd.end(dl);
}

void Thread::Frame(const SceneRing::Packet& packet) {
    const auto& dl = d.dispatch;
    const auto start = std::chrono::steady_clock::now();
    if (needs_swapchain && !Recreate()) {
        SceneRing::Release(packet.slot);
        return;
    }
    const u32 slot_index = next;
    Slot& slot = slots[slot_index];
    (void)d.device.waitForFences(slot.done, true, UINT64_MAX, dl);
    ReadTimes(slot_index);
    if (slot.scene != SceneRing::Slots) {
        SceneRing::Release(slot.scene);
        slot.scene = SceneRing::Slots;
    }

    u32 index = 0;
    if (!swapchain->Acquire(slot.acquired, index)) {
        needs_swapchain = true;
        SceneRing::Release(packet.slot);
        return;
    }
    (void)d.device.resetFences(slot.done, dl);
    (void)d.device.resetCommandPool(slot.pool, {}, dl);
    Record(slot, index, packet.frame);
    slot.scene = packet.slot;

    const vk::SemaphoreSubmitInfo wait{
        .semaphore = slot.acquired,
        .stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    };
    const vk::SemaphoreSubmitInfo signal{
        .semaphore = swapchain->Rendered(index),
        .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
    };
    const vk::CommandBufferSubmitInfo commands{.commandBuffer = slot.commands};
    const vk::SubmitInfo2 submit{
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commands,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    if (d.queue.submit2(submit, slot.done, dl) != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: the frame submit failed");
        SceneRing::Release(slot.scene);
        slot.scene = SceneRing::Slots;
        return;
    }
    next = (next + 1) % FramesInFlight;
    stats.cpu_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    stats.frames++;
    if (!swapchain->Present(index)) {
        needs_swapchain = true;
    }
}

void Thread::Report() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - stats.start).count();
    const u64 game_frames = Capture::Frames();
    const u64 skipped = SceneRing::Skipped();
    const double game_fps = static_cast<double>(game_frames - stats.game_frames) / seconds;
    const double frames = std::max(stats.frames, 1u);
    const double gpu_ms = stats.timed != 0 ? stats.gpu_ms / stats.timed : 0.0;
    LOG_INFO(Render_Vulkan,
             "GR2 PT renderer: {:.1f} game frames/s, {} drawn in {:.1f} s, {} game frames "
             "skipped, per frame {:.3f} ms on the render thread, {:.3f} ms on the GPU",
             game_fps, stats.frames, seconds, skipped - stats.skipped, stats.cpu_ms / frames,
             gpu_ms);
    Window::SetTitle(fmt::format("GR2 PT | game {:.0f} fps | render thread {:.2f} ms | GPU "
                                 "{:.2f} ms",
                                 game_fps, stats.cpu_ms / frames, gpu_ms));
    stats = {.game_frames = game_frames, .skipped = skipped, .start = now};
}

void Thread::Run() {
    Common::SetCurrentThreadName("GR2 PT Render");
    if (!Init()) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: the renderer did not start");
        return;
    }
    u64 seen = 0;
    stats.game_frames = Capture::Frames();
    for (;;) {
        SceneRing::Packet packet;
        if (!SceneRing::Take(seen, packet)) {
            continue;
        }
        seen = packet.frame;
        Frame(packet);
        if (stats.frames >= ReportFrames) {
            Report();
        }
    }
}

} // namespace

void Start() {
    std::thread([] {
        static Thread thread;
        thread.Run();
    }).detach();
}

} // namespace Gr2Pt::Renderer
