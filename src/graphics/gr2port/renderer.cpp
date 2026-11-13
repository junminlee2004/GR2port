// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/debug_files.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/stb.h"
#include "common/thread.h"
#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/capture_math.h"
#include "graphics/gr2port/character_glass.h"
#include "graphics/gr2port/contours.h"
#include "graphics/gr2port/deformers.h"
#include "graphics/gr2port/device.h"
#include "graphics/gr2port/device_lost.h"
#include "graphics/gr2port/effects.h"
#include "graphics/gr2port/enemies.h"
#include "graphics/gr2port/fog.h"
#include "graphics/gr2port/frame_stats.h"
#include "graphics/gr2port/glass.h"
#include "graphics/gr2port/imgui_relay.h"
#include "graphics/gr2port/look.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/meshes.h"
#include "graphics/gr2port/motion_blur.h"
#include "graphics/gr2port/movies.h"
#include "graphics/gr2port/overlays.h"
#include "graphics/gr2port/particles.h"
#include "graphics/gr2port/pass_times.h"
#include "graphics/gr2port/photo.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/point_lights.h"
#include "graphics/gr2port/renderer.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/scene_ring.h"
#include "graphics/gr2port/self_translucent.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/shaders/character_depth_frag.h"
#include "graphics/gr2port/shaders/character_frag.h"
#include "graphics/gr2port/shaders/character_vert.h"
#include "graphics/gr2port/shaders/interior_frag.h"
#include "graphics/gr2port/shaders/lambert_frag.h"
#include "graphics/gr2port/shaders/lambert_vert.h"
#include "graphics/gr2port/shaders/vision_frag.h"
#include "graphics/gr2port/shadow_trace.h"
#include "graphics/gr2port/shadows.h"
#include "graphics/gr2port/splash.h"
#include "graphics/gr2port/swapchain.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/ui.h"
#include "graphics/gr2port/upscaler.h"
#include "graphics/gr2port/vegetation.h"
#include "graphics/gr2port/velocity.h"
#include "graphics/gr2port/water.h"
#include "graphics/gr2port/window.h"
#include "graphics/gr2port/world.h"

namespace Gr2Port::Renderer {

namespace {

constexpr u32 FramesInFlight = 2;
// GPU timestamps per frame: the start, after the uploads, after the deformers, after each cascade,
// after the fog's cells, the surfaces, the lines, the occlusion, the light, the forward draws, the
// fog's apply, the meter and the bloom, and the end.
constexpr u32 Timestamps = 13 + Scene::Cascades;
constexpr u32 FogTimestamp = 3 + Scene::Cascades;
constexpr u32 SurfacesTimestamp = FogTimestamp + 1;
constexpr u32 LinesTimestamp = SurfacesTimestamp + 1;
constexpr u32 OcclusionTimestamp = SurfacesTimestamp + 2;
constexpr u32 LightTimestamp = SurfacesTimestamp + 3;
constexpr u32 ForwardTimestamp = SurfacesTimestamp + 4;
constexpr u32 FogApplyTimestamp = SurfacesTimestamp + 5;
constexpr u32 MeterTimestamp = SurfacesTimestamp + 6;
constexpr u32 BloomTimestamp = SurfacesTimestamp + 7;
// GPU intervals per frame: the whole frame, the deformers, each cascade, the fog's cells, the
// surfaces, the lines, the occlusion, the light, the forward draws, the fog's apply, the meter, the
// bloom and the picture.
constexpr u32 Intervals = 12 + Scene::Cascades;
constexpr u32 DeformerInterval = 1;
constexpr u32 FirstCascadeInterval = 2;
constexpr u32 FogInterval = FirstCascadeInterval + Scene::Cascades;
constexpr u32 SurfacesInterval = FogInterval + 1;
constexpr u32 LinesInterval = SurfacesInterval + 1;
constexpr u32 OcclusionInterval = SurfacesInterval + 2;
constexpr u32 LightInterval = SurfacesInterval + 3;
constexpr u32 ForwardInterval = SurfacesInterval + 4;
constexpr u32 FogApplyInterval = SurfacesInterval + 5;
constexpr u32 MeterInterval = SurfacesInterval + 6;
constexpr u32 BloomInterval = SurfacesInterval + 7;
constexpr u32 PictureInterval = SurfacesInterval + 8;
constexpr u32 ReportFrames = 600;
constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;
// The scene's size until a world frame names the game's target for its picture: the game's own.
constexpr vk::Extent2D DefaultSceneExtent{1920, 1080};
// A frame with more indices than this is not drawn: its items are not what the capture expects.
// BaseFrameIndices is the limit for the base room, and it grows with the room for draws.
constexpr u64 BaseFrameIndices = 48u << 20;
u64 MaxFrameIndices() {
    return BaseFrameIndices * Scene::g_room.draws / Scene::BaseRoom.draws;
}

struct CharacterPush {
    u64 draws;
    u64 characters;
    u64 decals;
    u64 light;
    std::array<float, 2> target_inverse;
    u64 point_lights;
    u64 light_grid;
    u32 glow_pass;
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

// The stats at the F10 counter's last publication, which the next one takes the difference to.
struct Published {
    std::chrono::steady_clock::time_point time{};
    u64 game_frames = 0;
    u32 frames = 0;
    u32 timed = 0;
    double work_ms = 0.0;
    double wait_ms = 0.0;
    double gpu_ms = 0.0;
    u64 capture_ns = 0;
};
constexpr auto PublishPeriod = std::chrono::milliseconds{500};

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
    // Each drawn frame's gap to the game's frame before it and to the last present, in
    // microseconds: how evenly frames arrive and leave.
    std::array<u32, ReportFrames> game_gaps{};
    std::array<u32, ReportFrames> present_gaps{};
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
    u64 leaves = 0;
    u64 leaf_shadows = 0;
    u64 dropped_leaves = 0;
    u64 grass = 0;
    u64 dropped_grass = 0;
    u64 branch_jobs = 0;
    u64 dropped_branch_jobs = 0;
    u64 branches = 0;
    u64 dropped_branches = 0;
    u64 world_decals = 0;
    u64 dropped_world_decals = 0;
    u64 glass_materials = 0;
    u64 dropped_glass = 0;
    u64 metaballs = 0;
    u64 dropped_metaballs = 0;
    u64 enemies = 0;
    u64 dropped_enemies = 0;
    u64 character_glass = 0;
    u64 dropped_character_glass = 0;
    u64 point_lights = 0;
    // Frames whose point lights ShadowTrace shadowed.
    u64 traced = 0;
    u64 particle_draws = 0;
    u64 particle_runs = 0;
    u64 particle_unported = 0;
    u64 particle_dropped = 0;
    u64 particle_refused = 0;
    u64 particle_live_effects = 0;
    u64 particle_capture_ns = 0;
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
    void RecordWorld(vk::CommandBuffer cmd, u32 slot_index, const Scene::Header& header,
                     const SceneRing::Packet& packet, u64 offset);
    void Record(Slot& slot, u32 index, const SceneRing::Packet& packet,
                const Scene::Header& header);
    void ReadTimes(u32 slot_index);
    void LogScene(const Scene::Header& header, const SceneRing::Packet& packet);
    void LogShadows(const Scene::Header& header, const SceneRing::Packet& packet);
    void LogDecals(const Scene::Header& header, const SceneRing::Packet& packet);
    void LogLight(const SceneRing::Packet& packet);
    void DrawCharacters(vk::CommandBuffer cmd, Scene::DrawList list, const Pipeline& pipeline,
                        std::span<const vk::DescriptorSet> sets, const Scene::Header& header,
                        const SceneRing::Packet& packet, u64 offset, vk::Extent2D extent,
                        bool glow_pass = false);
    void MeasureMotion(const SceneRing::Packet& packet);
    void RecordSnapshot(vk::CommandBuffer cmd, vk::Image image);
    void SaveSnapshot();
    void Report();
    void PublishStats();
    bool Recreate();
    bool ResizeScene(vk::Extent2D extent);
    bool SizeScene(const Scene::Header& header, const u8* packet);

    Device d;
    std::unique_ptr<Memory> memory;
    Buffer scene_ring;
    std::unique_ptr<Swapchain> swapchain;
    std::array<Slot, FramesInFlight> slots;
    vk::QueryPool timestamps;
    // The size of the picture the scene's images make, 0 by 0 before they are made, and a size
    // they did not fit at. The scene draws at render_extent, which the upscaler makes smaller.
    vk::Extent2D scene_extent;
    vk::Extent2D refused_extent;
    vk::Extent2D render_extent;
    Image depth;
    // The characters' depth alone, for the ambient occlusion.
    Image character_depth;
    Pipeline character_pipeline;
    Pipeline character_depth_pipeline;
    Pipeline interior_pipeline;
    Pipeline interior_depth_pipeline;
    Pipeline lambert_pipeline;
    Pipeline lambert_depth_pipeline;
    Pipeline vision_pipeline;
    Pipeline character_glow_pipeline;
    u32 next = 0;
    // The number of the next frame to submit; frames up to frame - FramesInFlight are done once
    // the frame's slot is free.
    u64 frame = FramesInFlight;
    // The camera of the last frame whose world was drawn, which the histories hold.
    Scene::Camera last_camera;
    bool camera_drawn = false;
    bool logged_scene = false;
    bool logged_shadows = false;
    bool logged_decals = false;
    bool logged_refusal = false;
    // With --debug-files, each report saves the next frame to a PNG in the log folder.
    Buffer snapshot;
    vk::Extent2D snapshot_extent;
    vk::Format snapshot_format = vk::Format::eUndefined;
    bool snapshot_wanted = false;
    bool needs_swapchain = false;
    Stats stats;
    Published published;
    // The launcher's graphics settings, read once at start.
    Settings::Graphics graphics = Settings::Get().graphics;
};

void Barrier(const Device& d, vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from,
             vk::ImageLayout to, vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
             vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         d.dispatch);
}

bool Thread::Init() {
    SDL_Window* const window = Window::Show();
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
    if (graphics.pass_times) {
        PassTimes::Start(d, FramesInFlight);
    }
    memory = std::make_unique<Memory>(d);
    if (!memory->Create()) {
        return false;
    }
    // The window shows the game's boot picture while the pipelines below are built.
    swapchain = std::make_unique<Swapchain>(d);
    Splash::Show(d, *memory, *swapchain, slots[0].pool, slots[0].commands, slots[0].done,
                 slots[0].acquired);
    // A level of detail setting at its finest puts several times the items in the game's frame.
    Scene::g_room = Settings::FinestDetail(graphics) ? Scene::MostRoom : Scene::BaseRoom;
    LOG_INFO(Render_Vulkan,
             "GR2port scene ring: {} slots of {} MB, each with room for {} draws, {} casters, {} "
             "materials and {} layer materials",
             SceneRing::Slots, SceneRing::SlotBytes() >> 20, Scene::g_room.draws,
             Scene::g_room.casters, Scene::g_room.materials, Scene::g_room.layer_materials);
    scene_ring = memory->CreateBuffer(SceneRing::Slots * SceneRing::SlotBytes(),
                                      vk::BufferUsageFlagBits::eStorageBuffer |
                                          vk::BufferUsageFlagBits::eIndirectBuffer |
                                          vk::BufferUsageFlagBits::eTransferSrc,
                                      Placement::Upload);
    if (scene_ring.mapped == nullptr || !Upscaler::Start(d, *memory) ||
        !Meshes::Start(d, *memory) || !Textures::Start(d, *memory) || !Ui::Start(d, *memory) ||
        !ImGuiRelay::Start(d, *memory) || !Shadows::Start(d, *memory) ||
        !Deformers::Start(d, *memory) || !Velocity::Start(d, *memory) ||
        !Look::Start(d, *memory, DepthFormat) ||
        !World::Start(d, *memory, DepthFormat, Look::SceneFormat) ||
        !Contours::Start(d, Look::SceneFormat, DepthFormat) ||
        !Fog::Start(d, *memory, Look::SceneFormat) ||
        !Glass::Start(d, *memory, Look::SceneFormat, DepthFormat) ||
        !SelfTranslucent::Start(d, *memory) ||
        !Particles::Start(d, *memory, Look::SceneFormat, DepthFormat) ||
        !PointLights::Start(d, *memory) || !ShadowTrace::Start(d, *memory) ||
        !MotionBlur::Start(d, *memory, Look::SceneFormat) ||
        !Effects::Start(d, Look::SceneFormat, DepthFormat) ||
        !Enemies::Start(d, Look::SceneFormat, DepthFormat) ||
        !CharacterGlass::Start(d, Look::SceneFormat, DepthFormat) ||
        !Water::Start(d, *memory, Look::SceneFormat, DepthFormat) ||
        !Vegetation::Start(d, *memory, World::SurfaceFormats, DepthFormat, Shadows::DepthFormat)) {
        return false;
    }
    const std::array set_layouts{Textures::SetLayout(), Shadows::SetLayout(),
                                 SelfTranslucent::SetLayout()};
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
    const std::array depth_set_layouts{Textures::SetLayout()};
    character_depth_pipeline =
        CreateGraphicsPipeline(d, {
                                      .vertex = CHARACTER_VERT,
                                      .fragment = CHARACTER_DEPTH_FRAG,
                                      .depth = DepthFormat,
                                      .cull = vk::CullModeFlagBits::eBack,
                                      .depth_test = true,
                                      .push_constant_bytes = sizeof(CharacterPush),
                                      .set_layouts = depth_set_layouts,
                                  });
    interior_pipeline =
        CreateGraphicsPipeline(d, {
                                      .vertex = CHARACTER_VERT,
                                      .fragment = INTERIOR_FRAG,
                                      .color = Look::SceneFormat,
                                      .depth = DepthFormat,
                                      .cull = vk::CullModeFlagBits::eBack,
                                      .depth_test = true,
                                      .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                      .push_constant_bytes = sizeof(CharacterPush),
                                      .set_layouts = set_layouts,
                                  });
    interior_depth_pipeline =
        CreateGraphicsPipeline(d, {
                                      .vertex = CHARACTER_VERT,
                                      .depth = DepthFormat,
                                      .cull = vk::CullModeFlagBits::eBack,
                                      .depth_test = true,
                                      .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                      .push_constant_bytes = sizeof(CharacterPush),
                                      .set_layouts = depth_set_layouts,
                                  });
    lambert_pipeline =
        CreateGraphicsPipeline(d, {
                                      .vertex = LAMBERT_VERT,
                                      .fragment = LAMBERT_FRAG,
                                      .color = Look::SceneFormat,
                                      .depth = DepthFormat,
                                      .cull = vk::CullModeFlagBits::eBack,
                                      .depth_test = true,
                                      .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                      .push_constant_bytes = sizeof(CharacterPush),
                                      .set_layouts = set_layouts,
                                  });
    lambert_depth_pipeline =
        CreateGraphicsPipeline(d, {
                                      .vertex = LAMBERT_VERT,
                                      .depth = DepthFormat,
                                      .cull = vk::CullModeFlagBits::eBack,
                                      .depth_test = true,
                                      .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                      .push_constant_bytes = sizeof(CharacterPush),
                                      .set_layouts = depth_set_layouts,
                                  });
    vision_pipeline = CreateGraphicsPipeline(d, {
                                                    .vertex = CHARACTER_VERT,
                                                    .fragment = VISION_FRAG,
                                                    .color = Look::SceneFormat,
                                                    .depth = DepthFormat,
                                                    .cull = vk::CullModeFlagBits::eBack,
                                                    .depth_test = true,
                                                    .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                                    .push_constant_bytes = sizeof(CharacterPush),
                                                    .set_layouts = set_layouts,
                                                });
    // The characters' glow, category 9, added into the bloom where the scene's depth shows it.
    character_glow_pipeline =
        CreateGraphicsPipeline(d, {
                                      .vertex = CHARACTER_VERT,
                                      .fragment = CHARACTER_FRAG,
                                      .color = Look::SceneFormat,
                                      .depth = DepthFormat,
                                      .cull = vk::CullModeFlagBits::eBack,
                                      .depth_test = true,
                                      .depth_write = false,
                                      .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                      .blend = &Look::GlowBlend,
                                      .push_constant_bytes = sizeof(CharacterPush),
                                      .set_layouts = set_layouts,
                                  });
    if (!character_pipeline.pipeline || !character_depth_pipeline.pipeline ||
        !interior_pipeline.pipeline || !interior_depth_pipeline.pipeline ||
        !lambert_pipeline.pipeline || !lambert_depth_pipeline.pipeline ||
        !vision_pipeline.pipeline || !character_glow_pipeline.pipeline) {
        return false;
    }
    SceneRing::Attach(scene_ring.mapped, scene_ring.address);
    needs_swapchain = true;
    SavePipelineCache(d);
    return true;
}

bool Thread::Recreate() {
    const Window::Size size = Window::PixelSize();
    if (d.device.waitIdle(d.dispatch) != vk::Result::eSuccess) {
        return false;
    }
    needs_swapchain = !swapchain->Create(size.width, size.height) ||
                      !Ui::Resize(swapchain->Format()) || !ImGuiRelay::Resize(swapchain->Format());
    if (!needs_swapchain) {
        // The screen's pipelines take the window's format.
        SavePipelineCache(d);
    }
    return !needs_swapchain;
}

// Makes the scene's images for a picture of output. When one is not made, the scene has none.
bool Thread::ResizeScene(vk::Extent2D output) {
    if (d.device.waitIdle(d.dispatch) != vk::Result::eSuccess) {
        return false;
    }
    scene_extent = vk::Extent2D{};
    // An upscaler that cannot run at these sizes leaves the scene at the picture's size.
    if (!Upscaler::Resize(Upscaler::RenderExtent(output), output)) {
        return false;
    }
    const vk::Extent2D extent = Upscaler::RenderExtent(output);
    for (Image* image : {&depth, &character_depth}) {
        memory->Destroy(*image);
        *image = memory->CreateImage(DepthFormat, extent,
                                     vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                         vk::ImageUsageFlagBits::eSampled |
                                         vk::ImageUsageFlagBits::eTransferSrc,
                                     vk::ImageAspectFlagBits::eDepth);
    }
    if (!depth.image || !character_depth.image ||
        !Velocity::Resize(extent, depth.view, character_depth.view)) {
        return false;
    }
    Upscaler::SetInputs(depth.image, depth.view, Velocity::MotionImage(), Velocity::MotionView());
    if (!Look::Resize(extent, output, Ui::TargetFormat, depth.view) ||
        !ShadowTrace::Resize(extent, depth.view) ||
        !MotionBlur::Resize(extent, Velocity::View(), depth.view, character_depth.view,
                            Look::SceneImage(), Look::SceneView()) ||
        !World::Resize(extent, depth.view, character_depth.view) || !Glass::Resize(extent) ||
        !Water::Resize(extent, Glass::BehindView()) || !SelfTranslucent::Resize(extent)) {
        return false;
    }
    Fog::Resize(depth.view);
    if (!Particles::Resize(depth.view, World::AlbedoView(), extent)) {
        return false;
    }
    Effects::Resize(depth.view, Glass::BehindView());
    scene_extent = output;
    render_extent = extent;
    // The picture's pipeline is made with the first images.
    SavePipelineCache(d);
    LOG_INFO(Render_Vulkan, "GR2port: the scene is drawn at {}x{} for a picture of {}x{}",
             extent.width, extent.height, output.width, output.height);
    return true;
}

// Gives the scene the size of the game's target for the world's picture, which the game's render
// resolution sets, or keeps its size on frames that name none. False when the scene has no images.
bool Thread::SizeScene(const Scene::Header& header, const u8* packet) {
    vk::Extent2D extent = scene_extent.width != 0 ? scene_extent : DefaultSceneExtent;
    if (header.ui.only == 0 && header.ui.world < std::min(header.ui.targets, Scene::MaxUiTargets)) {
        Scene::UiTarget target;
        std::memcpy(&target, packet + Scene::UiTargetsOffset + header.ui.world * sizeof(target),
                    sizeof(target));
        const vk::Extent2D world{target.width, target.height};
        if (world != refused_extent) {
            extent = world;
        }
    }
    if (extent == scene_extent || ResizeScene(extent)) {
        return true;
    }
    LOG_ERROR(Render_Vulkan, "GR2port: no room for the scene's images at {}x{}", extent.width,
              extent.height);
    refused_extent = extent;
    return extent != DefaultSceneExtent && ResizeScene(DefaultSceneExtent);
}

// Inside a rendering at extent: draws the packet's draws of a character list with pipeline, as
// the bloom's glow draws when glow_pass is set.
void Thread::DrawCharacters(vk::CommandBuffer cmd, Scene::DrawList list, const Pipeline& pipeline,
                            std::span<const vk::DescriptorSet> sets, const Scene::Header& header,
                            const SceneRing::Packet& packet, u64 offset, vk::Extent2D extent,
                            bool glow_pass) {
    const auto& dl = d.dispatch;
    const u32 count = header.draw_commands[static_cast<u32>(list)];
    if (count == 0) {
        return;
    }
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport = Upscaler::SceneViewport(extent, !glow_pass);
    const vk::Rect2D scissor{{0, 0}, extent};
    const CharacterPush push{
        .draws = packet.address + Scene::DrawsOffset,
        .characters = packet.address + Scene::CharactersOffset,
        .decals = packet.address + Scene::DecalsOffset,
        .light = packet.address + Scene::LightOffset,
        .target_inverse = {1.0f / static_cast<float>(extent.width),
                           -1.0f / static_cast<float>(extent.height)},
        .point_lights = packet.address + Scene::PointLightsOffset,
        .light_grid = PointLights::GridAddress(),
        .glow_pass = glow_pass ? 1u : 0u,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {}, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, scissor, dl);
    cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                      &push, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    cmd.drawIndexedIndirect(scene_ring.buffer,
                            offset + Scene::CommandsOffset() +
                                u64{Scene::FirstCommand(header, list)} * sizeof(Scene::Command),
                            count, sizeof(Scene::Command), dl);
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
    const bool refused = header.indices > MaxFrameIndices();
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
        reinterpret_cast<const Scene::Command*>(packet.data + Scene::CommandsOffset());
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
    std::array<float, 2> occlusion;
    std::memcpy(occlusion.data(),
                packet.data + Scene::LightOffset + offsetof(Scene::Light, occlusion_intensity),
                sizeof(occlusion));
    LOG_INFO(Render_Vulkan,
             "GR2port light: toward the light ({:.3f} {:.3f} {:.3f}); the game's key light "
             "colour ({:.3f} {:.3f} {:.3f} {:.3f}) direction of travel ({:.3f} {:.3f} {:.3f} "
             "{:.3f}); occlusion strength {:.4f}, program value {:.3f}",
             toward[0], toward[1], toward[2], color[0], color[1], color[2], color[3], key[0],
             key[1], key[2], key[3], occlusion[0], occlusion[1]);
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
        reinterpret_cast<const Scene::Caster*>(packet.data + Scene::CastersOffset());
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

// The game measures a frame's motion against the frame before it, which the render thread may not
// have drawn: the histories then hold an older frame. Before a frame's world draws, writes the
// packet's view_to_previous against the last frame drawn. MotionBlur blurs by the motion of the
// frame's own step, so its velocity scales take that step's share of the time since that frame.
void Thread::MeasureMotion(const SceneRing::Packet& packet) {
    Scene::Camera camera;
    std::memcpy(&camera, packet.data + Scene::CameraOffset, sizeof(camera));
    if (camera_drawn) {
        Capture::Matrix inverse_view;
        Capture::Matrix last_world_to_clip;
        std::memcpy(inverse_view.data(), camera.inverse_view, sizeof(inverse_view));
        std::memcpy(last_world_to_clip.data(), last_camera.world_to_clip,
                    sizeof(last_world_to_clip));
        const Capture::Matrix view_to_previous =
            Capture::Multiply(inverse_view, last_world_to_clip);
        std::memcpy(packet.data + Scene::LightOffset + offsetof(Scene::Light, view_to_previous),
                    view_to_previous.data(), sizeof(view_to_previous));
        // The clock stands still while the game is paused.
        const float elapsed = static_cast<float>(camera.time - last_camera.time);
        if (elapsed > 0.0f) {
            const float scale = camera.blur_scale * camera.time_step / elapsed;
            u8* const blur = packet.data + Scene::MotionBlurOffset;
            for (const size_t field : {offsetof(Scene::MotionBlur, velocity_scale),
                                       offsetof(Scene::MotionBlur, tile_velocity_scale),
                                       offsetof(Scene::MotionBlur, blur_velocity_scale)}) {
                std::memcpy(blur + field, &scale, sizeof(scale));
            }
        }
    }
    last_camera = camera;
    camera_drawn = true;
}

// The world's passes, from the deformers to the antialias, which leave the picture the composite
// reads.
void Thread::RecordWorld(vk::CommandBuffer cmd, u32 slot_index, const Scene::Header& header,
                         const SceneRing::Packet& packet, u64 offset) {
    const auto& dl = d.dispatch;
    const vk::Extent2D extent = render_extent;
    const bool drawn = header.draws != 0 && header.indices <= MaxFrameIndices();
    stats.refused += header.indices > MaxFrameIndices();
    if (drawn) {
        MeasureMotion(packet);
        Upscaler::BeginFrame(packet.data, frame, FramesInFlight);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Deformers);
        Deformers::Record(cmd, header, packet.address);
        PassTimes::Mark(cmd, PassTimes::Pass::Deformers);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Kernels);
        Vegetation::RecordKernels(cmd, header, packet.address);
        PassTimes::Mark(cmd, PassTimes::Pass::Kernels);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Simulation);
        Particles::RecordSimulation(cmd, header, packet.data, packet.address, scene_ring.buffer,
                                    offset);
        PassTimes::Mark(cmd, PassTimes::Pass::Simulation);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::PointLights);
        PointLights::Record(cmd, header, packet.address);
        Glass::Prepare(cmd);
        PassTimes::Mark(cmd, PassTimes::Pass::PointLights);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::SelfTranslucent);
        SelfTranslucent::Record(cmd, header, packet.data, packet.address, scene_ring.buffer,
                                offset);
        PassTimes::Mark(cmd, PassTimes::Pass::SelfTranslucent);
    }
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + 2, dl);
    }
    if (drawn) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Shadows);
        Shadows::Record(cmd, header, packet.data, packet.address, scene_ring.buffer, offset,
                        timestamps, Timestamps * slot_index + 3);
    } else if (timestamps) {
        for (u32 c = 0; c < Scene::Cascades; c++) {
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                                Timestamps * slot_index + 3 + c, dl);
        }
    }
    PassTimes::Mark(cmd, PassTimes::Pass::Shadows);
    if (drawn) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::FogVolumes);
        Fog::RecordVolumes(cmd, header, packet.address);
    }
    PassTimes::Mark(cmd, PassTimes::Pass::FogVolumes);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + FogTimestamp, dl);
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
    // The last frame's bloom read the depth.
    depth_barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  depth_tests | vk::PipelineStageFlagBits2::eFragmentShader,
                  vk::AccessFlagBits2::eNone, depth_tests, depth_writes);
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Surfaces);
    World::RecordSurfaces(cmd, header, packet.data, drawn, packet.address, scene_ring.buffer,
                          offset, depth);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + SurfacesTimestamp, dl);
    }

    depth_barrier(vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  vk::ImageLayout::eDepthStencilReadOnlyOptimal, depth_tests, depth_writes,
                  vk::PipelineStageFlagBits2::eFragmentShader |
                      vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
    if (drawn && graphics.outlines) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Lines);
        World::RecordLines(cmd, packet.address);
    }
    PassTimes::Mark(cmd, PassTimes::Pass::Lines);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + LinesTimestamp, dl);
    }
    if (drawn) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::CharacterDepth);
        // The last frame's occlusion sampled the characters' depth.
        Barrier(d, cmd, character_depth.image, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eDepthStencilAttachmentOptimal,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone, depth_tests,
                depth_writes, vk::ImageAspectFlagBits::eDepth);
        const vk::RenderingAttachmentInfo characters{
            .imageView = character_depth.view,
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = {.depthStencil = {.depth = 0.0f}},
        };
        cmd.beginRendering(
            {.renderArea = {{0, 0}, extent}, .layerCount = 1, .pDepthAttachment = &characters}, dl);
        const std::array depth_sets{Textures::Set()};
        DrawCharacters(cmd, Scene::DrawList::Lambert, lambert_depth_pipeline, depth_sets, header,
                       packet, offset, extent);
        Enemies::RecordDepth(cmd, header, packet.address, scene_ring.buffer, offset, extent);
        // The screens' depth takes the interiors' pipeline: the same vertex program and state.
        DrawCharacters(cmd, Scene::DrawList::Vision, interior_depth_pipeline, depth_sets, header,
                       packet, offset, extent);
        DrawCharacters(cmd, Scene::DrawList::Character, character_depth_pipeline, depth_sets,
                       header, packet, offset, extent);
        DrawCharacters(cmd, Scene::DrawList::Interior, interior_depth_pipeline, depth_sets, header,
                       packet, offset, extent);
        cmd.endRendering(dl);
        Barrier(d, cmd, character_depth.image, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                vk::ImageLayout::eDepthStencilReadOnlyOptimal, depth_tests, depth_writes,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead,
                vk::ImageAspectFlagBits::eDepth);
        PassTimes::Mark(cmd, PassTimes::Pass::CharacterDepth);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Occlusion);
        // Without ambient occlusion its images keep their clear: nothing is occluded.
        if (graphics.ambient_occlusion != Settings::Quality::Off) {
            World::RecordOcclusion(cmd, packet.address);
        }
    }
    PassTimes::Mark(cmd, PassTimes::Pass::Occlusion);
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Velocity);
    Velocity::Record(cmd, drawn, packet.address);
    const auto* light = reinterpret_cast<const Scene::Light*>(packet.data + Scene::LightOffset);
    ShadowTrace::Record(cmd, packet.address, drawn && light->shadow_trace != 0);
    PassTimes::Mark(cmd, PassTimes::Pass::Velocity);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + OcclusionTimestamp, dl);
    }
    // The last frame sampled the scene's colour in its meter and its picture.
    Barrier(d, cmd, Look::SceneImage(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eComputeShader |
                vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
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
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Light);
        World::RecordLight(cmd, packet.address);
    }
    cmd.endRendering(dl);
    PassTimes::Mark(cmd, PassTimes::Pass::Light);
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
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &forward,
        .pDepthAttachment = &depth_attachment,
    };
    cmd.beginRendering(rendering, dl);
    if (drawn) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Characters);
        const std::array sets{Textures::Set(), Shadows::Set(), SelfTranslucent::Set()};
        DrawCharacters(cmd, Scene::DrawList::Lambert, lambert_pipeline, sets, header, packet,
                       offset, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::Lambert);
        Enemies::RecordForward(cmd, header, packet.address, scene_ring.buffer, offset, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::EnemiesForward);
        DrawCharacters(cmd, Scene::DrawList::Vision, vision_pipeline, sets, header, packet, offset,
                       extent);
        DrawCharacters(cmd, Scene::DrawList::Character, character_pipeline, sets, header, packet,
                       offset, extent);
        DrawCharacters(cmd, Scene::DrawList::Interior, interior_pipeline, sets, header, packet,
                       offset, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::Characters);
    }
    if (header.sky != 0) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Sky);
        Look::RecordSky(cmd, packet.address);
    }
    PassTimes::Mark(cmd, PassTimes::Pass::Sky);
    if (drawn) {
        Enemies::RecordAlpha(cmd, header, packet.address, scene_ring.buffer, offset, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::EnemiesAlpha);
        CharacterGlass::Record(cmd, header, packet.data, packet.address, scene_ring.buffer, offset,
                               extent);
        PassTimes::Mark(cmd, PassTimes::Pass::CharacterGlass);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Contours);
        if (graphics.outlines) {
            Contours::Record(cmd, header, packet.address, scene_ring.buffer, offset, extent);
        }
    }
    cmd.endRendering(dl);
    PassTimes::Mark(cmd, PassTimes::Pass::Contours);
    // The fog's low path draws its particles before the fog: its opaque pass with the opaque
    // draws, the others after TransparentBg. Depth is readable while they draw.
    const auto low_particles = [&](Particles::Group group) {
        if (!drawn || !Particles::HasDraws(header, packet.data, group)) {
            return;
        }
        depth_barrier(vk::ImageLayout::eDepthStencilAttachmentOptimal,
                      vk::ImageLayout::eDepthStencilReadOnlyOptimal, depth_tests, depth_writes,
                      depth_tests | vk::PipelineStageFlagBits2::eFragmentShader |
                          vk::PipelineStageFlagBits2::eTransfer,
                      vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits2::eShaderSampledRead |
                          vk::AccessFlagBits2::eTransferRead);
        Particles::RecordDraws(cmd, header, packet.data, packet.address, scene_ring.buffer, offset,
                               Look::SceneView(), depth, extent, group);
        depth_barrier(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                      vk::ImageLayout::eDepthStencilAttachmentOptimal,
                      depth_tests | vk::PipelineStageFlagBits2::eFragmentShader,
                      vk::AccessFlagBits2::eNone, depth_tests, depth_writes);
    };
    low_particles(Particles::Group::LowOpaque);
    PassTimes::Mark(cmd, PassTimes::Pass::LowOpaque);
    if (drawn) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::EffectsBehind);
        Effects::RecordTransparentBg(cmd, header, packet.data, packet.address, scene_ring.buffer,
                                     offset, Look::SceneView(), depth, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::EffectsBg);
        low_particles(Particles::Group::LowTransparent);
        PassTimes::Mark(cmd, PassTimes::Pass::LowTransparent);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Glass);
        Glass::Record(cmd, header, packet.address, scene_ring.buffer, offset, depth, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::Glass);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Water);
        Water::Record(cmd, header, packet.address, scene_ring.buffer, offset, Look::SceneView(),
                      depth, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::Water);
    }
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + ForwardTimestamp, dl);
    }
    // The fog and the bloom read the depth.
    depth_barrier(vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  vk::ImageLayout::eDepthStencilReadOnlyOptimal, depth_tests, depth_writes,
                  vk::PipelineStageFlagBits2::eFragmentShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
    if (drawn) {
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::EffectsInFront);
        Effects::RecordTransparentFg(cmd, header, packet.data, packet.address, scene_ring.buffer,
                                     offset, Look::SceneView(), depth.view, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::EffectsFg);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::FogApply);
        // The fog blends over the forward draws' colour.
        const vk::MemoryBarrier2 forward_barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                             vk::AccessFlagBits2::eColorAttachmentWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &forward_barrier}, dl);
        const vk::RenderingAttachmentInfo fogged{
            .imageView = Look::SceneView(),
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        cmd.beginRendering({.renderArea = {{0, 0}, extent},
                            .layerCount = 1,
                            .colorAttachmentCount = 1,
                            .pColorAttachments = &fogged},
                           dl);
        Fog::RecordApply(cmd, packet.address, extent);
        cmd.endRendering(dl);
        PassTimes::Mark(cmd, PassTimes::Pass::FogApply);
        // The game's particle passes, after its fog.
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::Particles);
        Particles::RecordDraws(cmd, header, packet.data, packet.address, scene_ring.buffer, offset,
                               Look::SceneView(), depth, extent, Particles::Group::AfterFog);
        PassTimes::Mark(cmd, PassTimes::Pass::Particles);
        DeviceLost::Mark(d, cmd, DeviceLost::Pass::EffectsTopMost);
        Effects::RecordTopMost(cmd, header, packet.data, packet.address, scene_ring.buffer, offset,
                               Look::SceneView(), depth.view, extent);
        PassTimes::Mark(cmd, PassTimes::Pass::EffectsTopMost);
    }
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + FogApplyTimestamp, dl);
    }

    Barrier(
        d, cmd, Look::SceneImage(), vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead);
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Meter);
    Look::RecordMeter(cmd, packet.address);
    PassTimes::Mark(cmd, PassTimes::Pass::Meter);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + MeterTimestamp, dl);
    }
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Bloom);
    const std::array glow_sets{Textures::Set(), Shadows::Set(), SelfTranslucent::Set()};
    Look::RecordBloom(cmd, header, packet.address, scene_ring.buffer, offset,
                      [&](vk::CommandBuffer glow_cmd, vk::Extent2D glow_extent) {
                          DrawCharacters(glow_cmd, Scene::DrawList::CharacterGlow,
                                         character_glow_pipeline, glow_sets, header, packet, offset,
                                         glow_extent, true);
                      });
    PassTimes::Mark(cmd, PassTimes::Pass::Bloom);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + BloomTimestamp, dl);
    }

    if (drawn && graphics.motion_blur != Settings::Quality::Off) {
        MotionBlur::Record(cmd, packet.address);
    }
    PassTimes::Mark(cmd, PassTimes::Pass::MotionBlur);
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Antialias);
    Look::RecordAntialias(cmd, packet.address);
    PassTimes::Mark(cmd, PassTimes::Pass::Antialias);
}

void Thread::Record(Slot& slot, u32 index, const SceneRing::Packet& packet,
                    const Scene::Header& header) {
    const vk::CommandBuffer cmd = slot.commands;
    const auto& dl = d.dispatch;
    const u32 slot_index = static_cast<u32>(&slot - slots.data());
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::FrameStart);
    if (timestamps) {
        cmd.resetQueryPool(timestamps, Timestamps * slot_index, Timestamps, dl);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, timestamps,
                            Timestamps * slot_index, dl);
    }
    PassTimes::Begin(cmd, slot_index, header);
    memory->Refresh(frame);
    Meshes::Update(cmd, frame, frame - FramesInFlight);
    Textures::Update(cmd, frame, frame - FramesInFlight);
    Deformers::Update(cmd, frame, frame - FramesInFlight, header.frame);
    LogScene(header, packet);
    const u64 offset = u64{packet.slot} * SceneRing::SlotBytes();
    Ui::Prepare(cmd, header, packet.data, frame);
    Textures::Refresh(cmd, frame,
                      std::span{header.dynamic}.first(
                          std::min(header.dynamic_textures, Scene::MaxDynamicTextures)));
    Movies::Collect();
    PassTimes::Mark(cmd, PassTimes::Pass::Uploads);
    if (timestamps) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                            Timestamps * slot_index + 1, dl);
    }
    if (header.ui.only == 0) {
        RecordWorld(cmd, slot_index, header, packet, offset);
        // The game's Composite draws into its back buffer, or into the view's own target.
        const vk::Extent2D world = Ui::BeginWorld(cmd, header);
        if (world.width != 0) {
            DeviceLost::Mark(d, cmd, DeviceLost::Pass::Composite);
            Look::RecordComposite(cmd, packet.address, world);
            Ui::EndWorld(cmd);
        }
        PassTimes::Mark(cmd, PassTimes::Pass::Composite);
    } else if (timestamps) {
        // A frame of the screen drawing alone: its world passes take no time.
        for (u32 t = 2; t < Timestamps - 1; t++) {
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                                Timestamps * slot_index + t, dl);
        }
    }
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Ui);
    Ui::Record(cmd, header, packet.data, packet.address, scene_ring.buffer, offset);
    Photo::Record(*memory, cmd, header, slot_index);
    ImGuiRelay::Update(cmd, frame, frame - FramesInFlight);
    PassTimes::Mark(cmd, PassTimes::Pass::Ui);

    const vk::Image image = swapchain->Image(index);
    Barrier(d, cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    // Black around a back buffer of another shape than the window.
    const vk::RenderingAttachmentInfo picture{
        .imageView = swapchain->View(index),
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = {.color = vk::ClearColorValue{.float32 = std::array<float, 4>{0, 0, 0, 1}}},
    };
    cmd.beginRendering({.renderArea = {{0, 0}, swapchain->Extent()},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &picture},
                       dl);
    DeviceLost::Mark(d, cmd, DeviceLost::Pass::Present);
    Ui::RecordPresent(cmd, header, swapchain->Extent());
    ImGuiRelay::Record(cmd, swapchain->Extent());
    cmd.endRendering(dl);
    if (snapshot_wanted) {
        snapshot_wanted = false;
        if (header.ui.only == 0) {
            LogLight(packet);
            Fog::Log(packet.data);
            Look::LogAndReadMeter(cmd, packet.data);
        }
        if (Common::DebugFiles()) {
            RecordSnapshot(cmd, swapchain->Image(index));
            slot.snapshot = snapshot.mapped != nullptr;
        }
    }
    stats.draws += header.draws;
    stats.materials += header.materials;
    stats.dropped_materials += header.dropped_materials;
    stats.contours += header.contours;
    stats.dropped_contours += header.dropped_contours;
    stats.leaves += header.leaf_commands;
    for (const u32 commands : header.leaf_shadow_commands) {
        stats.leaf_shadows += commands;
    }
    stats.dropped_leaves += header.dropped_leaves;
    stats.grass += header.grass;
    stats.dropped_grass += header.dropped_grass;
    stats.branch_jobs += header.branch_jobs;
    stats.dropped_branch_jobs += header.dropped_branch_jobs;
    stats.branches += header.branches;
    stats.dropped_branches += header.dropped_branches;
    stats.world_decals += header.world_decals;
    stats.dropped_world_decals += header.dropped_world_decals;
    stats.glass_materials += header.glass_materials;
    stats.dropped_glass += header.dropped_glass;
    stats.metaballs += header.metaballs;
    stats.dropped_metaballs += header.dropped_metaballs;
    stats.enemies += header.enemies;
    stats.dropped_enemies += header.dropped_enemies;
    stats.character_glass += header.character_glass;
    stats.dropped_character_glass += header.dropped_character_glass;
    stats.point_lights += header.point_lights;
    stats.traced +=
        reinterpret_cast<const Scene::Light*>(packet.data + Scene::LightOffset)->shadow_trace != 0;
    stats.particle_draws += header.particles.draws;
    stats.particle_runs += header.particles.runs;
    stats.particle_unported += header.particles.unported;
    stats.particle_dropped += header.particles.dropped;
    stats.particle_refused += header.particles.refused;
    stats.particle_live_effects += header.particles.live_effects;
    stats.particle_capture_ns += header.particles.capture_ns;
    stats.dropped += header.dropped;
    stats.capture_ns += header.capture_ns;
    for (u32 part = 0; part < stats.capture_parts_ns.size(); part++) {
        stats.capture_parts_ns[part] += header.capture_parts_ns[part];
    }
    stats.captures[stats.frames] = header.capture_ns;
    stats.game_gaps[stats.frames] = header.frame_gap_ns / 1000;
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
    PassTimes::Mark(cmd, PassTimes::Pass::Present);
    (void)cmd.end(dl);
}

void Thread::Frame(const SceneRing::Packet& packet) {
    const auto& dl = d.dispatch;
    const auto start = std::chrono::steady_clock::now();
    // One read of the header: the packet is write-combined.
    Scene::Header header;
    std::memcpy(&header, packet.data, sizeof(header));
    if ((needs_swapchain && !Recreate()) || !SizeScene(header, packet.data)) {
        SceneRing::Release(packet.slot);
        return;
    }
    const u32 slot_index = next;
    Slot& slot = slots[slot_index];
    const auto wait_start = std::chrono::steady_clock::now();
    DeviceLost::Check(d, d.device.waitForFences(slot.done, true, UINT64_MAX, dl));
    const auto wait_end = std::chrono::steady_clock::now();
    ReadTimes(slot_index);
    PassTimes::Collect(slot_index);
    Photo::Collect(slot_index);
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
    Record(slot, index, packet, header);
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
    const vk::Result submitted = d.queue.submit2(submit, slot.done, dl);
    DeviceLost::Check(d, submitted);
    if (submitted != vk::Result::eSuccess) {
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
    static auto last_present = std::chrono::steady_clock::now();
    const auto presenting = std::chrono::steady_clock::now();
    stats.present_gaps[stats.frames] = static_cast<u32>(
        std::chrono::duration_cast<std::chrono::microseconds>(presenting - last_present).count());
    last_present = presenting;
    stats.frames++;
    if (!swapchain->Present(index)) {
        needs_swapchain = true;
    }
}

void Thread::Report() {
    PassTimes::Report();
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
             "{:.3f} {:.3f} {:.3f}, fog {:.3f}, surfaces {:.3f}, lines {:.3f}, occlusion {:.3f}, "
             "light {:.3f}, forward {:.3f}, fog apply {:.3f}, meter {:.3f}, bloom {:.3f}, picture "
             "{:.3f}; shortest {:.3f}, deformers {:.3f}, cascades {:.3f} {:.3f} {:.3f}, fog "
             "{:.3f}, surfaces {:.3f}, lines {:.3f}, occlusion {:.3f}, light {:.3f}, forward "
             "{:.3f}, fog apply {:.3f}, meter {:.3f}, bloom {:.3f}, picture {:.3f})",
             game_fps, stats.frames, seconds, skipped - stats.skipped, stats.work_ms / frames,
             stats.wait_ms / frames, gpu_ms, stats.gpu_ms[DeformerInterval] / timed,
             stats.gpu_ms[FirstCascadeInterval] / timed,
             stats.gpu_ms[FirstCascadeInterval + 1] / timed,
             stats.gpu_ms[FirstCascadeInterval + 2] / timed, stats.gpu_ms[FogInterval] / timed,
             stats.gpu_ms[SurfacesInterval] / timed, stats.gpu_ms[LinesInterval] / timed,
             stats.gpu_ms[OcclusionInterval] / timed, stats.gpu_ms[LightInterval] / timed,
             stats.gpu_ms[ForwardInterval] / timed, stats.gpu_ms[FogApplyInterval] / timed,
             stats.gpu_ms[MeterInterval] / timed, stats.gpu_ms[BloomInterval] / timed,
             stats.gpu_ms[PictureInterval] / timed, stats.gpu_min_ms[0],
             stats.gpu_min_ms[DeformerInterval], stats.gpu_min_ms[FirstCascadeInterval],
             stats.gpu_min_ms[FirstCascadeInterval + 1], stats.gpu_min_ms[FirstCascadeInterval + 2],
             stats.gpu_min_ms[FogInterval], stats.gpu_min_ms[SurfacesInterval],
             stats.gpu_min_ms[LinesInterval], stats.gpu_min_ms[OcclusionInterval],
             stats.gpu_min_ms[LightInterval], stats.gpu_min_ms[ForwardInterval],
             stats.gpu_min_ms[FogApplyInterval], stats.gpu_min_ms[MeterInterval],
             stats.gpu_min_ms[BloomInterval], stats.gpu_min_ms[PictureInterval]);
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
        "vertices), {} missed unloads, {} lost handles, {:.1f} of {} million indices held",
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
        meshes.missed_unloads, meshes.lost_handles,
        static_cast<double>(meshes.indices_held) / (1 << 20), meshes.index_capacity >> 20);
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
    LOG_INFO(Render_Vulkan,
             "GR2port texture streaming: quality {}, budget {} MB, {:.1f} MB held, {} textures "
             "read in the last 2 s ({:.1f} MB at full size), {:.1f} MB for all at full size, {} "
             "with fewer levels; {} made smaller, {} larger ({:.1f} MB uploaded)",
             textures.quality, textures.limit_bytes >> 20,
             static_cast<double>(textures.allocated_bytes) / (1 << 20), textures.used_textures,
             static_cast<double>(textures.used_bytes) / (1 << 20),
             static_cast<double>(textures.full_bytes) / (1 << 20), textures.reduced,
             textures.shrinks, textures.grows,
             static_cast<double>(textures.grow_bytes) / (1 << 20));
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
    LOG_INFO(Render_Vulkan,
             "GR2port vegetation: per frame {:.1f} leaf draws, {:.1f} leaf shadow draws, {:.1f} "
             "leaf items not drawn, {:.1f} grass instances, {:.1f} grass items not drawn, {:.1f} "
             "branch jobs, {:.1f} Branch records not run, {:.1f} ribbons, {:.1f} ribbon items not "
             "drawn",
             static_cast<double>(stats.leaves) / frames,
             static_cast<double>(stats.leaf_shadows) / frames,
             static_cast<double>(stats.dropped_leaves) / frames,
             static_cast<double>(stats.grass) / frames,
             static_cast<double>(stats.dropped_grass) / frames,
             static_cast<double>(stats.branch_jobs) / frames,
             static_cast<double>(stats.dropped_branch_jobs) / frames,
             static_cast<double>(stats.branches) / frames,
             static_cast<double>(stats.dropped_branches) / frames);
    LOG_INFO(Render_Vulkan, "GR2port world decals: per frame {:.1f} drawn, {:.1f} not drawn",
             static_cast<double>(stats.world_decals) / frames,
             static_cast<double>(stats.dropped_world_decals) / frames);
    LOG_INFO(Render_Vulkan, "GR2port glass: per frame {:.1f} drawn, {:.1f} not drawn",
             static_cast<double>(stats.glass_materials) / frames,
             static_cast<double>(stats.dropped_glass) / frames);
    LOG_INFO(Render_Vulkan, "GR2port metaballs: per frame {:.1f} drawn, {:.1f} not drawn",
             static_cast<double>(stats.metaballs) / frames,
             static_cast<double>(stats.dropped_metaballs) / frames);
    LOG_INFO(Render_Vulkan, "GR2port enemies: per frame {:.1f} drawn, {:.1f} not drawn",
             static_cast<double>(stats.enemies) / frames,
             static_cast<double>(stats.dropped_enemies) / frames);
    LOG_INFO(Render_Vulkan, "GR2port character glass: per frame {:.1f} drawn, {:.1f} not drawn",
             static_cast<double>(stats.character_glass) / frames,
             static_cast<double>(stats.dropped_character_glass) / frames);
    LOG_INFO(Render_Vulkan, "GR2port point lights: per frame {:.1f}, traced in {} of {} frames",
             static_cast<double>(stats.point_lights) / frames, stats.traced, stats.frames);
    LOG_INFO(Render_Vulkan,
             "GR2port particles: per frame {:.1f} live effects, {:.1f} draws in {:.1f} runs, "
             "{:.1f} items not ported, {:.1f} dropped, {:.3f} ms capture, {} frames refused",
             static_cast<double>(stats.particle_live_effects) / frames,
             static_cast<double>(stats.particle_draws) / frames,
             static_cast<double>(stats.particle_runs) / frames,
             static_cast<double>(stats.particle_unported) / frames,
             static_cast<double>(stats.particle_dropped) / frames,
             static_cast<double>(stats.particle_capture_ns) * 1e-6 / frames,
             stats.particle_refused);
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
    const auto log_gaps = [&](const char* what, std::array<u32, ReportFrames>& gaps) {
        const std::span sorted = std::span{gaps}.first(stats.frames);
        std::ranges::sort(sorted);
        const auto over = [&](u32 ms) {
            return sorted.end() - std::ranges::upper_bound(sorted, ms * 1000);
        };
        LOG_INFO(Render_Vulkan,
                 "GR2port pacing: {} median {:.2f} ms, 99% {:.2f}, longest {:.2f}; {} over 20 ms, "
                 "{} over 33 ms, {} over 50 ms, {} over 90 ms",
                 what, sorted[sorted.size() / 2] / 1000.0,
                 sorted[sorted.size() * 99 / 100] / 1000.0, sorted.back() / 1000.0, over(20),
                 over(33), over(50), over(90));
    };
    log_gaps("game frames", stats.game_gaps);
    log_gaps("presents", stats.present_gaps);
    stats = {.game_frames = game_frames, .skipped = skipped, .start = now};
    published = {.time = now, .game_frames = game_frames};
    snapshot_wanted = true;
}

void Thread::PublishStats() {
    const auto now = std::chrono::steady_clock::now();
    if (now - published.time < PublishPeriod) {
        return;
    }
    const u64 game_frames = Capture::Frames();
    const u64 capture_ns = stats.capture_ns + stats.particle_capture_ns;
    if (published.time != std::chrono::steady_clock::time_point{} &&
        stats.frames > published.frames) {
        const double seconds = std::chrono::duration<double>(now - published.time).count();
        const double frames = stats.frames - published.frames;
        const u32 timed = stats.timed - published.timed;
        FrameStats::Publish({
            .drawing = true,
            .game_fps = static_cast<double>(game_frames - published.game_frames) / seconds,
            .gpu_ms = timed > 0 ? (stats.gpu_ms[0] - published.gpu_ms) / timed : 0.0,
            .render_ms = (stats.work_ms - published.work_ms) / frames,
            .wait_ms = (stats.wait_ms - published.wait_ms) / frames,
            .capture_ms = static_cast<double>(capture_ns - published.capture_ns) * 1e-6 / frames,
        });
    }
    published = {now,           game_frames,   stats.frames,    stats.timed,
                 stats.work_ms, stats.wait_ms, stats.gpu_ms[0], capture_ns};
}

void Thread::Run() {
    Common::SetCurrentThreadName("GR2port Render");
    const auto started = std::chrono::steady_clock::now();
    const bool ready = Init();
    PipelineBuildDone();
    if (!ready) {
        Splash::End();
        LOG_ERROR(Render_Vulkan, "GR2port: the renderer did not start");
        return;
    }
    LogOverlays();
    u64 seen = 0;
    stats.game_frames = Capture::Frames();
    bool drawn = false;
    for (;;) {
        SceneRing::Packet packet;
        if (!SceneRing::Take(seen, packet)) {
            continue;
        }
        seen = packet.frame;
        // The splash has the queue and the swapchain until the first frame.
        Splash::End();
        Frame(packet);
        if (!drawn && stats.frames > 0) {
            drawn = true;
            LOG_INFO(Render_Vulkan, "GR2port: first frame drawn {} ms after the renderer started",
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started)
                         .count());
        }
        PublishStats();
        if (stats.frames >= ReportFrames) {
            Report();
        }
    }
}

} // namespace

void Start() {
    // The game starts once Init has built its pipelines.
    PipelineBuildStarted();
    std::thread([] {
        static Thread thread;
        thread.Run();
    }).detach();
}

} // namespace Gr2Port::Renderer
