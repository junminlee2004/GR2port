// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/stb.h"
#include "common/thread.h"
#include "video_core/gr2_pt/capture.h"
#include "video_core/gr2_pt/deformers.h"
#include "video_core/gr2_pt/device.h"
#include "video_core/gr2_pt/memory.h"
#include "video_core/gr2_pt/meshes.h"
#include "video_core/gr2_pt/pipeline.h"
#include "video_core/gr2_pt/renderer.h"
#include "video_core/gr2_pt/scene.h"
#include "video_core/gr2_pt/scene_ring.h"
#include "video_core/gr2_pt/shaders/mesh_frag.h"
#include "video_core/gr2_pt/shaders/mesh_vert.h"
#include "video_core/gr2_pt/shadows.h"
#include "video_core/gr2_pt/swapchain.h"
#include "video_core/gr2_pt/textures.h"
#include "video_core/gr2_pt/window.h"

namespace Gr2Pt::Renderer {

namespace {

constexpr u32 FramesInFlight = 2;
// GPU timestamps per frame: the start, after the uploads, after the deformers, after each cascade
// and the end.
constexpr u32 Timestamps = 4 + Scene::Cascades;
// GPU intervals per frame: the whole frame, the deformers, each cascade and the main view.
constexpr u32 Intervals = 3 + Scene::Cascades;
constexpr u32 DeformerInterval = 1;
constexpr u32 FirstCascadeInterval = 2;
constexpr u32 MainInterval = Intervals - 1;
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
    // The frame's image is copied to the snapshot buffer.
    bool snapshot = false;
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
    std::array<double, Intervals> gpu_ms{};
    // The shortest of each interval. The GPU runs other processes' work, such as GR2fork's, in
    // time slices, and an interval that spans a slice counts it.
    std::array<double, Intervals> gpu_min_ms = [] {
        std::array<double, Intervals> shortest;
        shortest.fill(1e9);
        return shortest;
    }();
    std::array<u64, Scene::Cascades> caster_indices{};
    u64 draws = 0;
    u64 dropped = 0;
    u64 capture_ns = 0;
    u64 untextured = 0;
    u64 skinned = 0;
    u64 deformed = 0;
    u64 deformed_waiting = 0;
    u64 skin_jobs = 0;
    u64 dropped_skins = 0;
    u64 palette_rows = 0;
    std::array<u64, Scene::ItemCategories> category_items{};
    u64 casters = 0;
    u64 skinned_casters = 0;
    u64 dropped_casters = 0;
    u64 deformed_casters = 0;
    u64 caster_draws = 0;
    u64 alpha_casters = 0;
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
    void LogScene(const Scene::Header& header, const SceneRing::Packet& packet);
    void LogShadows(const Scene::Header& header, const SceneRing::Packet& packet);
    void LogLight(const SceneRing::Packet& packet);
    void RecordSnapshot(vk::CommandBuffer cmd, vk::Image image);
    void SaveSnapshot();
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
    bool logged_shadows = false;
    bool logged_refusal = false;
    // Each report saves the next frame to a PNG in the log folder.
    Buffer snapshot;
    vk::Extent2D snapshot_extent;
    vk::Format snapshot_format = vk::Format::eUndefined;
    bool snapshot_wanted = false;
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
            .queryCount = Timestamps * FramesInFlight,
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
    if (scene_ring.mapped == nullptr || !Meshes::Start(d, *memory) ||
        !Textures::Start(d, *memory) || !Shadows::Start(d, *memory) ||
        !Deformers::Start(d, *memory)) {
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
        const std::array set_layouts{Textures::SetLayout(), Shadows::SetLayout()};
        DestroyPipeline(d, mesh_pipeline);
        mesh_pipeline = CreateGraphicsPipeline(d, {
                                                      .vertex = MESH_VERT,
                                                      .fragment = MESH_FRAG,
                                                      .color = swapchain->Format(),
                                                      .depth = DepthFormat,
                                                      .depth_test = true,
                                                      .push_constant_bytes = 2 * sizeof(u64),
                                                      .set_layouts = set_layouts,
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
    std::array<u64, Timestamps> ticks{};
    const vk::Result result = d.device.getQueryPoolResults(
        timestamps, Timestamps * slot_index, Timestamps, sizeof(ticks), ticks.data(), sizeof(u64),
        vk::QueryResultFlagBits::e64, d.dispatch);
    if (result == vk::Result::eSuccess && std::ranges::is_sorted(ticks)) {
        const double ms = d.timestamp_period * 1e-6;
        for (u32 i = 0; i < Intervals; i++) {
            const u64 elapsed = i == 0 ? ticks[Timestamps - 1] - ticks[0] : ticks[i + 1] - ticks[i];
            stats.gpu_ms[i] += static_cast<double>(elapsed) * ms;
            stats.gpu_min_ms[i] = std::min(stats.gpu_min_ms[i], static_cast<double>(elapsed) * ms);
        }
        stats.timed++;
    }
}

void Thread::RecordSnapshot(vk::CommandBuffer cmd, vk::Image image) {
    const vk::Extent2D extent = swapchain->Extent();
    const u64 bytes = u64{extent.width} * extent.height * 4;
    if (snapshot.size < bytes) {
        memory->Destroy(snapshot);
        snapshot =
            memory->CreateBuffer(bytes, vk::BufferUsageFlagBits::eTransferDst, Placement::Readback);
        if (snapshot.mapped == nullptr) {
            return;
        }
    }
    Barrier(d, cmd, image, vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eTransferSrcOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eCopy,
            vk::AccessFlagBits2::eTransferRead);
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {extent.width, extent.height, 1},
    };
    cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, snapshot.buffer, region,
                          d.dispatch);
    Barrier(d, cmd, image, vk::ImageLayout::eTransferSrcOptimal,
            vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eCopy,
            vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    snapshot_extent = extent;
    snapshot_format = swapchain->Format();
}

// Writes the snapshot as pt_frame.png in the log folder, off the render thread.
void Thread::SaveSnapshot() {
    const vk::Extent2D extent = snapshot_extent;
    const bool bgra = snapshot_format == vk::Format::eB8G8R8A8Unorm ||
                      snapshot_format == vk::Format::eB8G8R8A8Srgb;
    std::vector<u8> pixels(snapshot.mapped,
                           snapshot.mapped + u64{extent.width} * extent.height * 4);
    std::thread([pixels = std::move(pixels), extent, bgra]() mutable {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            if (bgra) {
                std::swap(pixels[i], pixels[i + 2]);
            }
            pixels[i + 3] = 0xff;
        }
        std::vector<u8> png;
        stbi_write_png_to_func(
            [](void* context, void* data, int size) {
                auto* out = static_cast<std::vector<u8>*>(context);
                const auto* bytes = static_cast<const u8*>(data);
                out->insert(out->end(), bytes, bytes + size);
            },
            &png, static_cast<int>(extent.width), static_cast<int>(extent.height), 4, pixels.data(),
            static_cast<int>(extent.width * 4));
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "pt_frame.png";
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Create};
        if (file.IsOpen()) {
            file.WriteRaw<u8>(png.data(), png.size());
        }
    }).detach();
}

// Logs the first packet with draws, and the first one too large to draw.
void Thread::LogScene(const Scene::Header& header, const SceneRing::Packet& packet) {
    LogShadows(header, packet);
    const bool refused = header.indices > MaxFrameIndices;
    if (header.draws == 0 || (logged_scene && (!refused || logged_refusal))) {
        return;
    }
    logged_scene = true;
    logged_refusal |= refused;
    LOG_INFO(Render_Vulkan,
             "GR2 PT: game frame {}: {} draws, {} indices, the largest {}, {} items not drawn{}",
             header.frame, header.draws, header.indices, header.max_indices, header.dropped,
             refused ? "; too many indices, not drawn" : "");
    const auto* draws = reinterpret_cast<const Scene::Draw*>(packet.data + Scene::DrawsOffset);
    const auto* commands =
        reinterpret_cast<const Scene::Command*>(packet.data + Scene::CommandsOffset);
    for (u32 i = 0; i < std::min(header.draws, 4u); i++) {
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

// Logs the light the main view uses, once per report: the direction toward the key light derived
// from the first cascade, and the game's key light.
void Thread::LogLight(const SceneRing::Packet& packet) {
    Scene::Shadows shadows;
    std::memcpy(&shadows, packet.data + Scene::ShadowsOffset, sizeof(shadows));
    const float* toward = shadows.light_direction;
    const float* color = shadows.key_light_color;
    const float* key = shadows.key_light_direction;
    LOG_INFO(Render_Vulkan,
             "GR2 PT light: toward the light ({:.3f} {:.3f} {:.3f}); the game's key light "
             "colour ({:.3f} {:.3f} {:.3f} {:.3f}) direction of travel ({:.3f} {:.3f} {:.3f} "
             "{:.3f})",
             toward[0], toward[1], toward[2], color[0], color[1], color[2], color[3], key[0],
             key[1], key[2], key[3]);
}

// Logs the cascades and the first casters of the first packet with casters.
void Thread::LogShadows(const Scene::Header& header, const SceneRing::Packet& packet) {
    if (logged_shadows || header.casters == 0) {
        return;
    }
    logged_shadows = true;
    const auto* shadows =
        reinterpret_cast<const Scene::Shadows*>(packet.data + Scene::ShadowsOffset);
    const float* light = shadows->light_direction;
    LOG_INFO(Render_Vulkan,
             "GR2 PT: game frame {}: cascades {:#x}, depth bias ({} {}) ({} {}) ({} {}), {} "
             "casters, lists {} {} {}, {} skinned, {} not drawable, light ({:.3f} {:.3f} "
             "{:.3f})",
             header.frame, header.cascades, header.depth_bias[0][0], header.depth_bias[0][1],
             header.depth_bias[1][0], header.depth_bias[1][1], header.depth_bias[2][0],
             header.depth_bias[2][1], header.casters, header.caster_commands[0],
             header.caster_commands[1], header.caster_commands[2], header.skinned_casters,
             header.dropped_casters, light[0], light[1], light[2]);
    for (u32 c = 0; c < Scene::Cascades; c++) {
        const float* vp = shadows->view_projection[c * Scene::CasterTables];
        LOG_INFO(Render_Vulkan,
                 "GR2 PT: cascade {} VP rows ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} "
                 "{:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f})",
                 c, vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10],
                 vp[11], vp[12], vp[13], vp[14], vp[15]);
    }
    const auto* casters =
        reinterpret_cast<const Scene::Caster*>(packet.data + Scene::CastersOffset);
    for (u32 i = 0; i < std::min(header.casters, 2u); i++) {
        const float* w = casters[i].world;
        LOG_INFO(Render_Vulkan,
                 "GR2 PT: caster {}: vertices {:#x} stride {} position {:#x} table {}, world row 3 "
                 "({:.2f} {:.2f} {:.2f} {:.2f})",
                 i, casters[i].vertices, casters[i].stride, casters[i].position, casters[i].table,
                 w[12], w[13], w[14], w[15]);
    }
}

void Thread::Record(Slot& slot, u32 index, const SceneRing::Packet& packet) {
    const vk::CommandBuffer cmd = slot.commands;
    const auto& dl = d.dispatch;
    const u32 slot_index = static_cast<u32>(&slot - slots.data());
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    if (timestamps) {
        cmd.resetQueryPool(timestamps, Timestamps * slot_index, Timestamps, dl);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, timestamps,
                            Timestamps * slot_index, dl);
    }
    // One read of the header: the packet is write-combined.
    Scene::Header header;
    std::memcpy(&header, packet.data, sizeof(header));
    memory->Refresh(frame);
    Meshes::Update(cmd, frame, frame - FramesInFlight);
    Textures::Update(cmd, frame, frame - FramesInFlight);
    Deformers::Update(cmd, frame, frame - FramesInFlight, header.frame);
    LogScene(header, packet);
    const bool drawn = header.draws != 0 && header.indices <= MaxFrameIndices;
    stats.refused += header.indices > MaxFrameIndices;
    const u64 offset = u64{packet.slot} * SceneRing::SlotBytes;
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + 1, dl);
    }
    if (drawn) {
        Deformers::Record(cmd, header, packet.address);
    }
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + 2, dl);
    }
    if (drawn) {
        Shadows::Record(cmd, header, packet.address, scene_ring.buffer, offset, timestamps,
                        Timestamps * slot_index + 3);
    } else if (timestamps) {
        for (u32 c = 0; c < Scene::Cascades; c++) {
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                                Timestamps * slot_index + 3 + c, dl);
        }
    }

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
        const std::array<u64, 2> addresses{packet.address + Scene::DrawsOffset,
                                           packet.address + Scene::ShadowsOffset};
        const std::array sets{Textures::Set(), Shadows::Set()};
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, mesh_pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, mesh_pipeline.layout, 0, sets, {},
                               dl);
        cmd.setViewport(0, viewport, dl);
        cmd.setScissor(0, scissor, dl);
        cmd.pushConstants(mesh_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                          sizeof(addresses), addresses.data(), dl);
        cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
        cmd.drawIndexedIndirect(scene_ring.buffer, offset + Scene::CommandsOffset, header.draws,
                                sizeof(Scene::Command), dl);
    }
    cmd.endRendering(dl);
    if (snapshot_wanted) {
        snapshot_wanted = false;
        LogLight(packet);
        RecordSnapshot(cmd, swapchain->Image(index));
        slot.snapshot = snapshot.mapped != nullptr;
    }
    stats.draws += header.draws;
    stats.dropped += header.dropped;
    stats.capture_ns += header.capture_ns;
    stats.untextured += header.untextured;
    stats.skinned += header.skinned;
    stats.deformed += header.deformed;
    stats.deformed_waiting += header.deformed_waiting;
    stats.skin_jobs += header.skin_jobs;
    stats.dropped_skins += header.dropped_skins;
    stats.palette_rows += header.palette_rows;
    for (u32 c = 0; c < Scene::ItemCategories; c++) {
        stats.category_items[c] += header.category_items[c];
    }
    stats.indices += header.indices;
    stats.max_indices = std::max(stats.max_indices, header.max_indices);
    stats.casters += header.casters;
    stats.skinned_casters += header.skinned_casters;
    stats.dropped_casters += header.dropped_casters;
    stats.deformed_casters += header.deformed_casters;
    for (u32 c = 0; c < Scene::Cascades; c++) {
        for (const u32 commands : header.caster_commands[c]) {
            stats.caster_draws += commands;
        }
        stats.alpha_casters += header.caster_commands[c][2] + header.caster_commands[c][3];
        stats.caster_indices[c] += header.caster_indices[c];
    }

    Barrier(d, cmd, image, vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::ePresentSrcKHR, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eNone,
            vk::AccessFlagBits2::eNone);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, timestamps,
                            Timestamps * slot_index + Timestamps - 1, dl);
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
    if (slot.snapshot) {
        slot.snapshot = false;
        SaveSnapshot();
    }
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
    const double timed = std::max(stats.timed, 1u);
    const double gpu_ms = stats.gpu_ms[0] / timed;
    const Meshes::Stats meshes = Meshes::TakeStats();
    const Textures::Stats textures = Textures::TakeStats();
    const Deformers::Stats deformers = Deformers::TakeStats();
    const Memory::Budget budget = memory->DeviceBudget();
    LOG_INFO(Render_Vulkan,
             "GR2 PT renderer: {:.1f} game frames/s, {} drawn in {:.1f} s, {} game frames "
             "skipped, per frame {:.3f} ms of render thread work, {:.3f} ms waiting for a "
             "frame slot and a swapchain image, {:.3f} ms on the GPU (deformers {:.3f}, cascades "
             "{:.3f} {:.3f} {:.3f}, main view {:.3f}; shortest {:.3f}, deformers {:.3f}, cascades "
             "{:.3f} {:.3f} {:.3f}, main view {:.3f})",
             game_fps, stats.frames, seconds, skipped - stats.skipped, stats.work_ms / frames,
             stats.wait_ms / frames, gpu_ms, stats.gpu_ms[DeformerInterval] / timed,
             stats.gpu_ms[FirstCascadeInterval] / timed,
             stats.gpu_ms[FirstCascadeInterval + 1] / timed,
             stats.gpu_ms[FirstCascadeInterval + 2] / timed, stats.gpu_ms[MainInterval] / timed,
             stats.gpu_min_ms[0], stats.gpu_min_ms[DeformerInterval],
             stats.gpu_min_ms[FirstCascadeInterval], stats.gpu_min_ms[FirstCascadeInterval + 1],
             stats.gpu_min_ms[FirstCascadeInterval + 2], stats.gpu_min_ms[MainInterval]);
    LOG_INFO(Render_Vulkan,
             "GR2 PT scene: per frame {:.0f} draws ({:.0f} skinned, {:.0f} deformed, {:.1f} KB "
             "of palettes), {:.0f} items not drawn and {:.0f} deformed ones waiting for their "
             "copy, {:.0f} indices (largest draw {}), {:.3f} ms of capture on the game's draw "
             "thread, {} frames not drawn for too many indices; {} meshes ({} skinned) in {:.1f} "
             "MB, {} waiting, {:.1f} MB uploaded, {} not drawable ({} with indices past their "
             "vertices), {} missed unloads, {} lost handles",
             static_cast<double>(stats.draws) / frames, static_cast<double>(stats.skinned) / frames,
             static_cast<double>(stats.deformed) / frames,
             static_cast<double>(stats.palette_rows) * 16 / 1024 / frames,
             static_cast<double>(stats.dropped) / frames,
             static_cast<double>(stats.deformed_waiting) / frames,
             static_cast<double>(stats.indices) / frames, stats.max_indices,
             static_cast<double>(stats.capture_ns) * 1e-6 / frames, stats.refused, meshes.meshes,
             meshes.skinned, static_cast<double>(meshes.resident_bytes) / (1 << 20), meshes.pending,
             static_cast<double>(meshes.uploaded_bytes) / (1 << 20), meshes.skipped,
             meshes.bad_indices, meshes.missed_unloads, meshes.lost_handles);
    LOG_INFO(Render_Vulkan,
             "GR2 PT textures: per frame {:.0f} draws with a colour texture not converted; {} "
             "textures in {:.1f} MB, {} waiting, {:.1f} MB uploaded of {:.1f} MB of data; not "
             "converted: {} not a single 2D image, {} tiling or format, {} larger than their data, "
             "{} no memory; {} missed destroys; {} meshes without texture coordinates",
             static_cast<double>(stats.untextured) / frames, textures.textures,
             static_cast<double>(textures.resident_bytes) / (1 << 20), textures.pending,
             static_cast<double>(textures.uploaded_bytes) / (1 << 20),
             static_cast<double>(textures.uploaded_data_bytes) / (1 << 20), textures.not_2d,
             textures.unsupported, textures.too_large, textures.failed, textures.missed_destroys,
             meshes.no_texcoord);
    LOG_INFO(Render_Vulkan,
             "GR2 PT shadows: per frame {:.0f} casters ({:.0f} skinned, {:.0f} deformed) in {:.0f} "
             "cascade draws ({:.0f} alpha-tested) of {:.0f} {:.0f} {:.0f} indices; {:.0f} not "
             "drawn",
             static_cast<double>(stats.casters) / frames,
             static_cast<double>(stats.skinned_casters) / frames,
             static_cast<double>(stats.deformed_casters) / frames,
             static_cast<double>(stats.caster_draws) / frames,
             static_cast<double>(stats.alpha_casters) / frames,
             static_cast<double>(stats.caster_indices[0]) / frames,
             static_cast<double>(stats.caster_indices[1]) / frames,
             static_cast<double>(stats.caster_indices[2]) / frames,
             static_cast<double>(stats.dropped_casters) / frames);
    std::string categories;
    for (u32 c = 0; c < Scene::ItemCategories; c++) {
        if (stats.category_items[c] != 0) {
            categories +=
                fmt::format(" {}:{:.0f}", c, static_cast<double>(stats.category_items[c]) / frames);
        }
    }
    LOG_INFO(Render_Vulkan, "GR2 PT categories: items per frame{}", categories);
    LOG_INFO(Render_Vulkan,
             "GR2 PT deformers: per frame {:.1f} skin jobs, {:.1f} Skin records not run; {} "
             "instance copies in {:.2f} MB, {} waiting, {} with a layout not their shape's, {} "
             "freed unused",
             static_cast<double>(stats.skin_jobs) / frames,
             static_cast<double>(stats.dropped_skins) / frames, deformers.instances,
             static_cast<double>(deformers.resident_bytes) / (1 << 20), deformers.pending,
             deformers.unsupported, deformers.retired);
    LOG_INFO(Render_Vulkan,
             "GR2 PT memory: device-local {} MB of a {} MB budget used by the process, {} MB by "
             "the renderer",
             budget.usage >> 20, budget.budget >> 20, budget.own >> 20);
    Window::SetTitle(fmt::format("GR2 PT | game {:.0f} fps | render thread {:.2f} ms | GPU "
                                 "{:.2f} ms",
                                 game_fps, stats.work_ms / frames, gpu_ms));
    stats = {.game_frames = game_frames, .skipped = skipped, .start = now};
    snapshot_wanted = true;
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
