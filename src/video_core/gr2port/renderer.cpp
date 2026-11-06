// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <span>
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
#include "video_core/gr2port/capture.h"
#include "video_core/gr2port/contours.h"
#include "video_core/gr2port/deformers.h"
#include "video_core/gr2port/device.h"
#include "video_core/gr2port/look.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/overlays.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/renderer.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/scene_ring.h"
#include "video_core/gr2port/shaders/character_frag.h"
#include "video_core/gr2port/shaders/character_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/swapchain.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/window.h"
#include "video_core/gr2port/world.h"

namespace Gr2Port::Renderer {

namespace {

constexpr u32 FramesInFlight = 2;
// GPU timestamps per frame: the start, after the uploads, after the deformers, after each cascade,
// after the surfaces, the lines, the light, the forward draws and the meter, and the end.
constexpr u32 Timestamps = 9 + Scene::Cascades;
constexpr u32 SurfacesTimestamp = 3 + Scene::Cascades;
constexpr u32 LinesTimestamp = SurfacesTimestamp + 1;
constexpr u32 LightTimestamp = SurfacesTimestamp + 2;
constexpr u32 ForwardTimestamp = SurfacesTimestamp + 3;
constexpr u32 MeterTimestamp = SurfacesTimestamp + 4;
// GPU intervals per frame: the whole frame, the deformers, each cascade, the surfaces, the lines,
// the light, the forward draws, the meter and the picture.
constexpr u32 Intervals = 8 + Scene::Cascades;
constexpr u32 DeformerInterval = 1;
constexpr u32 FirstCascadeInterval = 2;
constexpr u32 SurfacesInterval = FirstCascadeInterval + Scene::Cascades;
constexpr u32 LinesInterval = SurfacesInterval + 1;
constexpr u32 LightInterval = SurfacesInterval + 2;
constexpr u32 ForwardInterval = SurfacesInterval + 3;
constexpr u32 MeterInterval = SurfacesInterval + 4;
constexpr u32 PictureInterval = SurfacesInterval + 5;
constexpr u32 ReportFrames = 600;
constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;
// A frame with more indices than this is not drawn: its items are not what the capture expects.
constexpr u64 MaxFrameIndices = 48u << 20;

static_assert(Scene::PacketBytes <= SceneRing::SlotBytes);

struct CharacterPush {
    u64 draws;
    u64 characters;
    u64 decals;
    u64 light;
    std::array<float, 2> target_inverse;
};

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
    std::array<u64, static_cast<u32>(Scene::CapturePart::Count)> capture_parts_ns{};
    // Each frame's capture, for their median: work on other threads makes some much longer.
    std::array<u32, ReportFrames> captures{};
    u64 untextured = 0;
    u64 skinned = 0;
    u64 deformed = 0;
    u64 deformed_waiting = 0;
    u64 skin_jobs = 0;
    u64 dropped_skins = 0;
    u64 soft_body_jobs = 0;
    std::array<u64, static_cast<u32>(Scene::SoftBodyDrop::Count)> dropped_soft_bodies{};
    u64 spheres = 0;
    u64 decals = 0;
    u64 dropped_decals = 0;
    u64 materials = 0;
    u64 dropped_materials = 0;
    u64 contours = 0;
    u64 dropped_contours = 0;
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
    void LogDecals(const Scene::Header& header, const SceneRing::Packet& packet);
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
    Pipeline character_pipeline;
    u32 next = 0;
    // The number of the next frame to submit; frames up to frame - FramesInFlight are done once
    // the frame's slot is free.
    u64 frame = FramesInFlight;
    bool logged_scene = false;
    bool logged_shadows = false;
    bool logged_decals = false;
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
        !Deformers::Start(d, *memory) || !Look::Start(d, *memory, DepthFormat) ||
        !World::Start(d, *memory, DepthFormat, Look::SceneFormat) ||
        !Contours::Start(d, Look::SceneFormat, DepthFormat)) {
        return false;
    }
    const std::array set_layouts{Textures::SetLayout(), Shadows::SetLayout()};
    character_pipeline = CreateGraphicsPipeline(d, {
                                                       .vertex = CHARACTER_VERT,
                                                       .fragment = CHARACTER_FRAG,
                                                       .color = Look::SceneFormat,
                                                       .depth = DepthFormat,
                                                       .depth_test = true,
                                                       .cull = vk::CullModeFlagBits::eBack,
                                                       .push_constant_bytes = sizeof(CharacterPush),
                                                       .set_layouts = set_layouts,
                                                   });
    if (!character_pipeline.pipeline) {
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
                                vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                    vk::ImageUsageFlagBits::eSampled,
                                vk::ImageAspectFlagBits::eDepth);
    needs_swapchain = !depth.image || !Look::Resize(swapchain->Extent(), swapchain->Format()) ||
                      !World::Resize(swapchain->Extent(), depth.view);
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
    LogDecals(header, packet);
    const bool refused = header.indices > MaxFrameIndices;
    if (header.draws == 0 || (logged_scene && (!refused || logged_refusal))) {
        return;
    }
    logged_scene = true;
    logged_refusal |= refused;
    LOG_INFO(Render_Vulkan,
             "GR2port: game frame {}: {} draws, {} indices, the largest {}, {} items not drawn{}",
             header.frame, header.draws, header.indices, header.max_indices, header.dropped,
             refused ? "; too many indices, not drawn" : "");
    const auto* draws = reinterpret_cast<const Scene::Draw*>(packet.data + Scene::DrawsOffset);
    const auto* commands =
        reinterpret_cast<const Scene::Command*>(packet.data + Scene::CommandsOffset);
    for (u32 i = 0; i < std::min(header.draws, 4u); i++) {
        const Scene::Draw& draw = draws[commands[i].first_instance];
        const float* wvp = draw.world_view_projection;
        LOG_INFO(Render_Vulkan,
                 "GR2port: draw {}: vertices {:#x} stride {} position {:#x}, indices {} from {}, "
                 "WVP rows ({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f}) "
                 "({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f})",
                 i, draw.vertices, draw.stride, draw.position, commands[i].index_count,
                 commands[i].first_index, wvp[0], wvp[1], wvp[2], wvp[3], wvp[4], wvp[5], wvp[6],
                 wvp[7], wvp[8], wvp[9], wvp[10], wvp[11], wvp[12], wvp[13], wvp[14], wvp[15]);
    }
}

// Logs the first decals of the first packet that has some.
void Thread::LogDecals(const Scene::Header& header, const SceneRing::Packet& packet) {
    if (logged_decals || header.decals == 0) {
        return;
    }
    logged_decals = true;
    const auto* decals = reinterpret_cast<const Scene::Decal*>(packet.data + Scene::DecalsOffset);
    for (u32 i = 0; i < std::min(header.decals, 4u); i++) {
        const Scene::Decal& decal = decals[i];
        std::string text;
        for (u32 j = 0; j < 3; j++) {
            text += fmt::format(" ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) ({:.2f} {:.2f})",
                                decal.positions[j][0], decal.positions[j][1], decal.positions[j][2],
                                decal.targets[j][0], decal.targets[j][1], decal.targets[j][2],
                                decal.scales[j][0], decal.scales[j][1]);
        }
        LOG_INFO(Render_Vulkan,
                 "GR2port: decals {}: texture {}, colour alpha {}, gradation ({:.2f} {:.2f} {:.2f} "
                 "{:.2f}), position, target, scale of each:{}",
                 i, decal.texture, decal.color_alpha, decal.gradation[0], decal.gradation[1],
                 decal.gradation[2], decal.gradation[3], text);
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
             "GR2port light: toward the light ({:.3f} {:.3f} {:.3f}); the game's key light "
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
             "GR2port: game frame {}: cascades {:#x}, depth bias ({} {}) ({} {}) ({} {}), {} "
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
                 "GR2port: cascade {} VP rows ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} "
                 "{:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f})",
                 c, vp[0], vp[1], vp[2], vp[3], vp[4], vp[5], vp[6], vp[7], vp[8], vp[9], vp[10],
                 vp[11], vp[12], vp[13], vp[14], vp[15]);
    }
    const auto* casters =
        reinterpret_cast<const Scene::Caster*>(packet.data + Scene::CastersOffset);
    for (u32 i = 0; i < std::min(header.casters, 2u); i++) {
        const float* w = casters[i].world;
        LOG_INFO(
            Render_Vulkan,
            "GR2port: caster {}: vertices {:#x} stride {} position {:#x} table {}, world row 3 "
            "({:.2f} {:.2f} {:.2f} {:.2f})",
            i, casters[i].vertices, casters[i].stride, casters[i].position, casters[i].table, w[12],
            w[13], w[14], w[15]);
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

    const auto depth_barrier = [&](vk::ImageLayout from, vk::ImageLayout to,
                                   vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                                   vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .image = depth.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dl);
    };
    const vk::PipelineStageFlags2 depth_tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                                vk::PipelineStageFlagBits2::eLateFragmentTests;
    const vk::AccessFlags2 depth_writes = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                          vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    depth_barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  depth_tests, vk::AccessFlagBits2::eNone, depth_tests, depth_writes);
    World::RecordSurfaces(cmd, header, drawn, packet.address, scene_ring.buffer, offset,
                          depth.view);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + SurfacesTimestamp, dl);
    }

    depth_barrier(vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  vk::ImageLayout::eDepthStencilReadOnlyOptimal, depth_tests, depth_writes,
                  vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
    if (drawn) {
        World::RecordLines(cmd, packet.address);
    }
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + LinesTimestamp, dl);
    }
    // The last frame sampled the scene's colour in its meter and its picture.
    Barrier(d, cmd, Look::SceneImage(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eComputeShader |
                vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    const vk::Extent2D extent = swapchain->Extent();
    const vk::RenderingAttachmentInfo lit{
        .imageView = Look::SceneView(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = {.color =
                           vk::ClearColorValue{.float32 = std::array<float, 4>{header.background[0],
                                                                               header.background[1],
                                                                               header.background[2],
                                                                               0.0f}}},
    };
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &lit},
                       dl);
    if (drawn) {
        World::RecordLight(cmd, packet.address);
    }
    cmd.endRendering(dl);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + LightTimestamp, dl);
    }

    depth_barrier(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                  vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eNone, depth_tests, depth_writes);
    const vk::MemoryBarrier2 lit_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &lit_barrier}, dl);
    const vk::RenderingAttachmentInfo forward{
        .imageView = Look::SceneView(),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth.view,
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eDontCare,
    };
    const vk::RenderingInfo rendering{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &forward,
        .pDepthAttachment = &depth_attachment,
    };
    cmd.beginRendering(rendering, dl);
    const u32 character_draws = header.draw_commands[static_cast<u32>(Scene::DrawList::Character)];
    if (drawn && character_draws != 0) {
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
        const CharacterPush push{
            .draws = packet.address + Scene::DrawsOffset,
            .characters = packet.address + Scene::CharactersOffset,
            .decals = packet.address + Scene::DecalsOffset,
            .light = packet.address + Scene::LightOffset,
            .target_inverse = {1.0f / static_cast<float>(extent.width),
                               -1.0f / static_cast<float>(extent.height)},
        };
        const std::array sets{Textures::Set(), Shadows::Set()};
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, character_pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, character_pipeline.layout, 0, sets,
                               {}, dl);
        cmd.setViewport(0, viewport, dl);
        cmd.setScissor(0, scissor, dl);
        cmd.pushConstants(character_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                          sizeof(push), &push, dl);
        cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
        cmd.drawIndexedIndirect(scene_ring.buffer,
                                offset + Scene::CommandsOffset +
                                    u64{Scene::FirstCommand(header, Scene::DrawList::Character)} *
                                        sizeof(Scene::Command),
                                character_draws, sizeof(Scene::Command), dl);
    }
    if (header.sky != 0) {
        Look::RecordSky(cmd, packet.address);
    }
    if (drawn) {
        Contours::Record(cmd, header, packet.address, scene_ring.buffer, offset, extent);
    }
    cmd.endRendering(dl);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + ForwardTimestamp, dl);
    }

    Barrier(
        d, cmd, Look::SceneImage(), vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead);
    Look::RecordMeter(cmd, packet.address);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + MeterTimestamp, dl);
    }

    const vk::Image image = swapchain->Image(index);
    Barrier(d, cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    const vk::RenderingAttachmentInfo picture{
        .imageView = swapchain->View(index),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &picture},
                       dl);
    Look::RecordComposite(cmd, packet.address);
    cmd.endRendering(dl);
    if (snapshot_wanted) {
        snapshot_wanted = false;
        LogLight(packet);
        Look::LogAndReadMeter(cmd, packet.data);
        RecordSnapshot(cmd, swapchain->Image(index));
        slot.snapshot = snapshot.mapped != nullptr;
    }
    stats.draws += header.draws;
    stats.materials += header.materials;
    stats.dropped_materials += header.dropped_materials;
    stats.contours += header.contours;
    stats.dropped_contours += header.dropped_contours;
    stats.dropped += header.dropped;
    stats.capture_ns += header.capture_ns;
    for (u32 part = 0; part < stats.capture_parts_ns.size(); part++) {
        stats.capture_parts_ns[part] += header.capture_parts_ns[part];
    }
    stats.captures[stats.frames] = header.capture_ns;
    stats.untextured += header.untextured;
    stats.skinned += header.skinned;
    stats.deformed += header.deformed;
    stats.deformed_waiting += header.deformed_waiting;
    stats.skin_jobs += header.skin_jobs;
    stats.dropped_skins += header.dropped_skins;
    stats.soft_body_jobs += header.soft_body_jobs;
    for (u32 reason = 0; reason < stats.dropped_soft_bodies.size(); reason++) {
        stats.dropped_soft_bodies[reason] += header.dropped_soft_bodies[reason];
    }
    stats.spheres += header.spheres;
    stats.decals += header.decals;
    stats.dropped_decals += header.dropped_decals;
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
        LOG_ERROR(Render_Vulkan, "GR2port: the frame submit failed");
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
    const std::span captures = std::span{stats.captures}.first(stats.frames);
    std::ranges::nth_element(captures, captures.begin() + captures.size() / 2);
    const auto capture_part = [&](Scene::CapturePart part) {
        return static_cast<double>(stats.capture_parts_ns[static_cast<u32>(part)]) * 1e-6 / frames;
    };
    const Meshes::Stats meshes = Meshes::TakeStats();
    const Textures::Stats textures = Textures::TakeStats();
    const Deformers::Stats deformers = Deformers::TakeStats();
    const Memory::Budget budget = memory->DeviceBudget();
    LOG_INFO(Render_Vulkan,
             "GR2port renderer: {:.1f} game frames/s, {} drawn in {:.1f} s, {} game frames "
             "skipped, per frame {:.3f} ms of render thread work, {:.3f} ms waiting for a "
             "frame slot and a swapchain image, {:.3f} ms on the GPU (deformers {:.3f}, cascades "
             "{:.3f} {:.3f} {:.3f}, surfaces {:.3f}, lines {:.3f}, light {:.3f}, forward {:.3f}, "
             "meter {:.3f}, picture {:.3f}; shortest {:.3f}, deformers {:.3f}, cascades {:.3f} "
             "{:.3f} {:.3f}, surfaces {:.3f}, lines {:.3f}, light {:.3f}, forward {:.3f}, meter "
             "{:.3f}, picture {:.3f})",
             game_fps, stats.frames, seconds, skipped - stats.skipped, stats.work_ms / frames,
             stats.wait_ms / frames, gpu_ms, stats.gpu_ms[DeformerInterval] / timed,
             stats.gpu_ms[FirstCascadeInterval] / timed,
             stats.gpu_ms[FirstCascadeInterval + 1] / timed,
             stats.gpu_ms[FirstCascadeInterval + 2] / timed, stats.gpu_ms[SurfacesInterval] / timed,
             stats.gpu_ms[LinesInterval] / timed, stats.gpu_ms[LightInterval] / timed,
             stats.gpu_ms[ForwardInterval] / timed, stats.gpu_ms[MeterInterval] / timed,
             stats.gpu_ms[PictureInterval] / timed, stats.gpu_min_ms[0],
             stats.gpu_min_ms[DeformerInterval], stats.gpu_min_ms[FirstCascadeInterval],
             stats.gpu_min_ms[FirstCascadeInterval + 1], stats.gpu_min_ms[FirstCascadeInterval + 2],
             stats.gpu_min_ms[SurfacesInterval], stats.gpu_min_ms[LinesInterval],
             stats.gpu_min_ms[LightInterval], stats.gpu_min_ms[ForwardInterval],
             stats.gpu_min_ms[MeterInterval], stats.gpu_min_ms[PictureInterval]);
    LOG_INFO(
        Render_Vulkan,
        "GR2port scene: per frame {:.0f} draws ({:.0f} skinned, {:.0f} deformed, {:.1f} with "
        "decals, {:.1f} without room for theirs, {:.0f} with materials, {:.1f} without room "
        "for theirs, {:.1f} outlines, {:.1f} not drawn, {:.1f} KB of palettes), {:.0f} items "
        "not "
        "drawn and {:.0f} deformed ones waiting for their "
        "copy, {:.0f} indices (largest draw {}), {:.3f} ms of capture on the game's draw "
        "thread (median {:.3f}, shortest {:.3f}; constants {:.3f}, deformers {:.3f}, casters "
        "{:.3f}, world {:.3f}, characters {:.3f}), {} frames not drawn for too many indices; {} "
        "meshes ({} skinned) in {:.1f} "
        "MB, {} waiting, {:.1f} MB uploaded, {} not drawable ({} with indices past their "
        "vertices), {} missed unloads, {} lost handles",
        static_cast<double>(stats.draws) / frames, static_cast<double>(stats.skinned) / frames,
        static_cast<double>(stats.deformed) / frames, static_cast<double>(stats.decals) / frames,
        static_cast<double>(stats.dropped_decals) / frames,
        static_cast<double>(stats.materials) / frames,
        static_cast<double>(stats.dropped_materials) / frames,
        static_cast<double>(stats.contours) / frames,
        static_cast<double>(stats.dropped_contours) / frames,
        static_cast<double>(stats.palette_rows) * 16 / 1024 / frames,
        static_cast<double>(stats.dropped) / frames,
        static_cast<double>(stats.deformed_waiting) / frames,
        static_cast<double>(stats.indices) / frames, stats.max_indices,
        static_cast<double>(stats.capture_ns) * 1e-6 / frames,
        static_cast<double>(captures[captures.size() / 2]) * 1e-6,
        static_cast<double>(*std::ranges::min_element(captures)) * 1e-6,
        capture_part(Scene::CapturePart::Constants), capture_part(Scene::CapturePart::Deformers),
        capture_part(Scene::CapturePart::Casters), capture_part(Scene::CapturePart::World),
        capture_part(Scene::CapturePart::Characters), stats.refused, meshes.meshes, meshes.skinned,
        static_cast<double>(meshes.resident_bytes) / (1 << 20), meshes.pending,
        static_cast<double>(meshes.uploaded_bytes) / (1 << 20), meshes.skipped, meshes.bad_indices,
        meshes.missed_unloads, meshes.lost_handles);
    LOG_INFO(
        Render_Vulkan,
        "GR2port textures: per frame {:.0f} draws with a colour texture not converted; {} "
        "textures in {:.1f} MB, {} waiting, {:.1f} MB uploaded of {:.1f} MB of data; not "
        "converted: {} not a single 2D image or cube map, {} tiling or format, {} larger than "
        "their data, {} no memory; {} missed destroys; {} meshes without texture coordinates, {} "
        "without normals",
        static_cast<double>(stats.untextured) / frames, textures.textures,
        static_cast<double>(textures.resident_bytes) / (1 << 20), textures.pending,
        static_cast<double>(textures.uploaded_bytes) / (1 << 20),
        static_cast<double>(textures.uploaded_data_bytes) / (1 << 20), textures.not_2d,
        textures.unsupported, textures.too_large, textures.failed, textures.missed_destroys,
        meshes.no_texcoord, meshes.no_normal);
    LOG_INFO(
        Render_Vulkan,
        "GR2port shadows: per frame {:.0f} casters ({:.0f} skinned, {:.0f} deformed) in {:.0f} "
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
    LOG_INFO(Render_Vulkan, "GR2port categories: items per frame{}", categories);
    const auto dropped_soft_bodies = [&](Scene::SoftBodyDrop reason) {
        return static_cast<double>(stats.dropped_soft_bodies[static_cast<u32>(reason)]) / frames;
    };
    LOG_INFO(Render_Vulkan,
             "GR2port deformers: per frame {:.1f} skin jobs, {:.1f} Skin records not run, {:.1f} "
             "SoftBody jobs, SoftBody records not run: {:.1f} mesh not uploaded, {:.1f} copy not "
             "ready, {:.1f} record mismatch, {:.1f} constants, {:.1f} job cap; {:.1f} effector "
             "spheres; {} instance copies in {:.2f} MB, {} waiting, {} with a layout not their "
             "shape's, {} freed unused; {} meshes with cloth, {} with cloth data out of range",
             static_cast<double>(stats.skin_jobs) / frames,
             static_cast<double>(stats.dropped_skins) / frames,
             static_cast<double>(stats.soft_body_jobs) / frames,
             dropped_soft_bodies(Scene::SoftBodyDrop::Mesh),
             dropped_soft_bodies(Scene::SoftBodyDrop::Copy),
             dropped_soft_bodies(Scene::SoftBodyDrop::Record),
             dropped_soft_bodies(Scene::SoftBodyDrop::Constants),
             dropped_soft_bodies(Scene::SoftBodyDrop::JobCap),
             static_cast<double>(stats.spheres) / frames, deformers.instances,
             static_cast<double>(deformers.resident_bytes) / (1 << 20), deformers.pending,
             deformers.unsupported, deformers.retired, meshes.cloth, meshes.bad_cloth);
    LOG_INFO(Render_Vulkan,
             "GR2port memory: device-local {} MB of a {} MB budget used by the process, {} MB by "
             "the renderer",
             budget.usage >> 20, budget.budget >> 20, budget.own >> 20);
    Window::SetTitle(fmt::format("GR2port | game {:.0f} fps | render thread {:.2f} ms | GPU "
                                 "{:.2f} ms",
                                 game_fps, stats.work_ms / frames, gpu_ms));
    stats = {.game_frames = game_frames, .skipped = skipped, .start = now};
    snapshot_wanted = true;
}

void Thread::Run() {
    Common::SetCurrentThreadName("GR2port Render");
    if (!Init()) {
        LOG_ERROR(Render_Vulkan, "GR2port: the renderer did not start");
        return;
    }
    LogOverlays();
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

} // namespace Gr2Port::Renderer
