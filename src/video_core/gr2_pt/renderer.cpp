// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/gr2_pt/capture.h"
#include "video_core/gr2_pt/device.h"
#include "video_core/gr2_pt/memory.h"
#include "video_core/gr2_pt/meshes.h"
#include "video_core/gr2_pt/pipeline.h"
#include "video_core/gr2_pt/renderer.h"
#include "video_core/gr2_pt/scene.h"
#include "video_core/gr2_pt/scene_ring.h"
#include "video_core/gr2_pt/shaders/mesh_frag.h"
#include "video_core/gr2_pt/shaders/mesh_vert.h"
#include "video_core/gr2_pt/swapchain.h"
#include "video_core/gr2_pt/window.h"

namespace Gr2Pt::Renderer {

namespace {

constexpr u32 FramesInFlight = 2;
constexpr u32 ReportFrames = 600;
constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;
// A frame with more indices than this is not drawn: its items are not what the capture expects.
constexpr u64 MaxFrameIndices = 48u << 20;

static_assert(Scene::PacketBytes <= SceneRing::SlotBytes);

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
    // Render thread time spent recording and submitting, and spent waiting for a frame slot
    // and a swapchain image.
    double work_ms = 0.0;
    double wait_ms = 0.0;
    double gpu_ms = 0.0;
    u64 draws = 0;
    u64 dropped = 0;
    u64 capture_ns = 0;
    u64 indices = 0;
    u32 max_indices = 0;
    u32 refused = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

class Thread {
public:
    void Run();

private:
    bool Init();
    void Frame(const SceneRing::Packet& packet);
    void Record(Slot& slot, u32 index, const SceneRing::Packet& packet);
    void ReadTimes(u32 slot_index);
    void LogScene(const SceneRing::Packet& packet);
    void Report();
    bool Recreate();

    Device d;
    std::unique_ptr<Memory> memory;
    Buffer scene_ring;
    std::unique_ptr<Swapchain> swapchain;
    std::array<Slot, FramesInFlight> slots;
    vk::QueryPool timestamps;
    Image depth;
    Pipeline mesh_pipeline;
    vk::Format pipeline_format = vk::Format::eUndefined;
    u32 next = 0;
    // The number of the next frame to submit; frames up to frame - FramesInFlight are done once
    // the frame's slot is free.
    u64 frame = FramesInFlight;
    bool logged_scene = false;
    bool logged_refusal = false;
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
                                      vk::BufferUsageFlagBits::eStorageBuffer |
                                          vk::BufferUsageFlagBits::eIndirectBuffer,
                                      Placement::Upload);
    if (scene_ring.mapped == nullptr || !Meshes::Start(d, *memory)) {
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
    if (needs_swapchain) {
        return false;
    }
    memory->Destroy(depth);
    depth = memory->CreateImage(DepthFormat, swapchain->Extent(),
                                vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                vk::ImageAspectFlagBits::eDepth);
    if (pipeline_format != swapchain->Format()) {
        DestroyPipeline(d, mesh_pipeline);
        mesh_pipeline = CreateGraphicsPipeline(d, {
                                                      .vertex = MESH_VERT,
                                                      .fragment = MESH_FRAG,
                                                      .color = swapchain->Format(),
                                                      .depth = DepthFormat,
                                                      .depth_test = true,
                                                      .push_constant_bytes = sizeof(u64),
                                                  });
        pipeline_format = swapchain->Format();
    }
    needs_swapchain = !depth.image || !mesh_pipeline.pipeline;
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

// Logs the first packet with draws, and the first one too large to draw.
void Thread::LogScene(const SceneRing::Packet& packet) {
    const auto* header = reinterpret_cast<const Scene::Header*>(packet.data);
    const bool refused = header->indices > MaxFrameIndices;
    if (header->draws == 0 || (logged_scene && (!refused || logged_refusal))) {
        return;
    }
    logged_scene = true;
    logged_refusal |= refused;
    LOG_INFO(Render_Vulkan,
             "GR2 PT: game frame {}: {} draws, {} indices, the largest {}, {} items not drawn{}",
             header->frame, header->draws, header->indices, header->max_indices, header->dropped,
             refused ? "; too many indices, not drawn" : "");
    const auto* draws = reinterpret_cast<const Scene::Draw*>(packet.data + Scene::DrawsOffset);
    const auto* commands =
        reinterpret_cast<const Scene::Command*>(packet.data + Scene::CommandsOffset);
    for (u32 i = 0; i < std::min(header->draws, 4u); i++) {
        const float* wvp = draws[i].world_view_projection;
        LOG_INFO(Render_Vulkan,
                 "GR2 PT: draw {}: vertices {:#x} stride {} position {:#x}, indices {} from {}, "
                 "WVP rows ({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f}) "
                 "({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f})",
                 i, draws[i].vertices, draws[i].stride, draws[i].position, commands[i].index_count,
                 commands[i].first_index, wvp[0], wvp[1], wvp[2], wvp[3], wvp[4], wvp[5], wvp[6],
                 wvp[7], wvp[8], wvp[9], wvp[10], wvp[11], wvp[12], wvp[13], wvp[14], wvp[15]);
    }
}

void Thread::Record(Slot& slot, u32 index, const SceneRing::Packet& packet) {
    const vk::CommandBuffer cmd = slot.commands;
    const auto& dl = d.dispatch;
    const u32 slot_index = static_cast<u32>(&slot - slots.data());
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    if (timestamps) {
        cmd.resetQueryPool(timestamps, 2 * slot_index, 2, dl);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, timestamps, 2 * slot_index, dl);
    }
    Meshes::Update(cmd, frame, frame - FramesInFlight);

    const vk::Image image = swapchain->Image(index);
    Barrier(d, cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    const vk::ImageMemoryBarrier2 depth_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eLateFragmentTests,
        .srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests,
        .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .image = depth.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &depth_barrier},
                         dl);

    const vk::RenderingAttachmentInfo color{
        .imageView = swapchain->View(index),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = {.color = vk::ClearColorValue{.float32 = std::array<float, 4>{0.32f, 0.38f,
                                                                                    0.45f, 1.0f}}},
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth.view,
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eDontCare,
        .clearValue = {.depthStencil = {.depth = 0.0f}},
    };
    const vk::Extent2D extent = swapchain->Extent();
    const vk::RenderingInfo rendering{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = &depth_attachment,
    };
    cmd.beginRendering(rendering, dl);
    const auto* header = reinterpret_cast<const Scene::Header*>(packet.data);
    LogScene(packet);
    const bool drawn = header->draws != 0 && header->indices <= MaxFrameIndices;
    stats.refused += header->indices > MaxFrameIndices;
    if (drawn) {
        // A negative height keeps the game's y-up clip space.
        const vk::Viewport viewport{
            .x = 0.0f,
            .y = static_cast<float>(extent.height),
            .width = static_cast<float>(extent.width),
            .height = -static_cast<float>(extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        const vk::Rect2D scissor{{0, 0}, extent};
        const u64 draws = packet.address + Scene::DrawsOffset;
        const u64 offset = u64{packet.slot} * SceneRing::SlotBytes;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mesh_pipeline.pipeline, dl);
        cmd.setViewport(0, viewport, dl);
        cmd.setScissor(0, scissor, dl);
        cmd.pushConstants(mesh_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                          sizeof(draws), &draws, dl);
        cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
        cmd.drawIndexedIndirect(scene_ring.buffer, offset + Scene::CommandsOffset, header->draws,
                                sizeof(Scene::Command), dl);
    }
    cmd.endRendering(dl);
    stats.draws += header->draws;
    stats.dropped += header->dropped;
    stats.capture_ns += header->capture_ns;
    stats.indices += header->indices;
    stats.max_indices = std::max(stats.max_indices, header->max_indices);

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
    const auto wait_start = std::chrono::steady_clock::now();
    (void)d.device.waitForFences(slot.done, true, UINT64_MAX, dl);
    const auto wait_end = std::chrono::steady_clock::now();
    ReadTimes(slot_index);
    if (slot.scene != SceneRing::Slots) {
        SceneRing::Release(slot.scene);
        slot.scene = SceneRing::Slots;
    }

    u32 index = 0;
    const auto acquire_start = std::chrono::steady_clock::now();
    const bool acquired = swapchain->Acquire(slot.acquired, index);
    const auto acquire_end = std::chrono::steady_clock::now();
    if (!acquired) {
        needs_swapchain = true;
        SceneRing::Release(packet.slot);
        return;
    }
    (void)d.device.resetFences(slot.done, dl);
    (void)d.device.resetCommandPool(slot.pool, {}, dl);
    Record(slot, index, packet);
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
    frame++;
    using Ms = std::chrono::duration<double, std::milli>;
    const double waited =
        Ms(wait_end - wait_start).count() + Ms(acquire_end - acquire_start).count();
    stats.wait_ms += waited;
    stats.work_ms += Ms(std::chrono::steady_clock::now() - start).count() - waited;
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
    const Meshes::Stats meshes = Meshes::TakeStats();
    LOG_INFO(Render_Vulkan,
             "GR2 PT renderer: {:.1f} game frames/s, {} drawn in {:.1f} s, {} game frames "
             "skipped, per frame {:.3f} ms of render thread work, {:.3f} ms waiting for a "
             "frame slot and a swapchain image, {:.3f} ms on the GPU",
             game_fps, stats.frames, seconds, skipped - stats.skipped, stats.work_ms / frames,
             stats.wait_ms / frames, gpu_ms);
    LOG_INFO(Render_Vulkan,
             "GR2 PT scene: per frame {:.0f} draws, {:.0f} items not drawn, {:.0f} indices "
             "(largest draw {}), {:.3f} ms of capture on the game's draw thread, {} frames not "
             "drawn for too many indices; {} meshes in {:.1f} MB, {} waiting, {:.1f} MB "
             "uploaded, {} not drawable ({} with indices past their vertices), {} missed "
             "unloads, {} lost handles",
             static_cast<double>(stats.draws) / frames, static_cast<double>(stats.dropped) / frames,
             static_cast<double>(stats.indices) / frames, stats.max_indices,
             static_cast<double>(stats.capture_ns) * 1e-6 / frames, stats.refused, meshes.meshes,
             static_cast<double>(meshes.resident_bytes) / (1 << 20), meshes.pending,
             static_cast<double>(meshes.uploaded_bytes) / (1 << 20), meshes.skipped,
             meshes.bad_indices, meshes.missed_unloads, meshes.lost_handles);
    Window::SetTitle(fmt::format("GR2 PT | game {:.0f} fps | render thread {:.2f} ms | GPU "
                                 "{:.2f} ms",
                                 game_fps, stats.work_ms / frames, gpu_ms));
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
