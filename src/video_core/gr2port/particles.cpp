// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "video_core/gr2port/device_lost.h"
#include "video_core/gr2port/fog.h"
#include "video_core/gr2port/look.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/particles.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/point_lights.h"
#include "video_core/gr2port/self_translucent.h"
#include "video_core/gr2port/shaders/collision_backup_comp.h"
#include "video_core/gr2port/shaders/color_backup_comp.h"
#include "video_core/gr2port/shaders/emitter_color_comp.h"
#include "video_core/gr2port/shaders/gpu_emitter_spawn_comp.h"
#include "video_core/gr2port/shaders/gpu_object_update_comp.h"
#include "video_core/gr2port/shaders/gpu_particle_clear_comp.h"
#include "video_core/gr2port/shaders/gpu_particle_move_comp.h"
#include "video_core/gr2port/shaders/gpu_sample_mesh_comp.h"
#include "video_core/gr2port/shaders/gpu_sample_skinned_mesh_comp.h"
#include "video_core/gr2port/shaders/model_blur_frag.h"
#include "video_core/gr2port/shaders/model_blur_soft_frag.h"
#include "video_core/gr2port/shaders/model_distortion_frag.h"
#include "video_core/gr2port/shaders/model_distortion_soft_frag.h"
#include "video_core/gr2port/shaders/model_fog_frag.h"
#include "video_core/gr2port/shaders/model_fog_lit_frag.h"
#include "video_core/gr2port/shaders/model_fog_lit_soft_frag.h"
#include "video_core/gr2port/shaders/model_fog_soft_frag.h"
#include "video_core/gr2port/shaders/model_gbuffer_frag.h"
#include "video_core/gr2port/shaders/model_gbuffer_soft_frag.h"
#include "video_core/gr2port/shaders/model_lit_frag.h"
#include "video_core/gr2port/shaders/model_lit_soft_frag.h"
#include "video_core/gr2port/shaders/model_plain_frag.h"
#include "video_core/gr2port/shaders/model_plain_soft_frag.h"
#include "video_core/gr2port/shaders/model_shadow_vert.h"
#include "video_core/gr2port/shaders/model_vert.h"
#include "video_core/gr2port/shaders/particle_effect_frag.h"
#include "video_core/gr2port/shaders/particle_effect_shadow_frag.h"
#include "video_core/gr2port/shaders/particle_effect_shadow_vert.h"
#include "video_core/gr2port/shaders/particle_effect_vert.h"
#include "video_core/gr2port/shaders/particle_gem_frag.h"
#include "video_core/gr2port/shaders/particle_gem_shadow_vert.h"
#include "video_core/gr2port/shaders/particle_gem_vert.h"
#include "video_core/gr2port/shaders/particle_mesh_shadow_frag.h"
#include "video_core/gr2port/shaders/particle_surface_color_frag.h"
#include "video_core/gr2port/shaders/particle_surface_color_vert.h"
#include "video_core/gr2port/shaders/particle_surface_frag.h"
#include "video_core/gr2port/shaders/particle_surface_shadow_vert.h"
#include "video_core/gr2port/shaders/particle_surface_tangent_color_frag.h"
#include "video_core/gr2port/shaders/particle_surface_tangent_color_vert.h"
#include "video_core/gr2port/shaders/particle_surface_tangent_frag.h"
#include "video_core/gr2port/shaders/particle_surface_tangent_vert.h"
#include "video_core/gr2port/shaders/particle_surface_vert.h"
#include "video_core/gr2port/shaders/sprite_blur_frag.h"
#include "video_core/gr2port/shaders/sprite_blur_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_distortion_frag.h"
#include "video_core/gr2port/shaders/sprite_distortion_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_fog_frag.h"
#include "video_core/gr2port/shaders/sprite_fog_lit_frag.h"
#include "video_core/gr2port/shaders/sprite_fog_lit_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_fog_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_gbuffer_frag.h"
#include "video_core/gr2port/shaders/sprite_gbuffer_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_lit_frag.h"
#include "video_core/gr2port/shaders/sprite_lit_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_plain_frag.h"
#include "video_core/gr2port/shaders/sprite_plain_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_ribbon_shadow_vert.h"
#include "video_core/gr2port/shaders/sprite_ribbon_vert.h"
#include "video_core/gr2port/shaders/sprite_shadow_frag.h"
#include "video_core/gr2port/shaders/sprite_shadow_soft_frag.h"
#include "video_core/gr2port/shaders/sprite_shadow_vert.h"
#include "video_core/gr2port/shaders/sprite_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/world.h"

namespace Gr2Port::Particles {

namespace {

// The programs' groups: 64 threads wide.
constexpr u32 GroupThreads = 64;
// The library's buffers: gpuDescription and gpuMaterials.
constexpr u64 DescriptionBytes = u64{0x8000} * 0x100;
constexpr u64 MaterialBytes = u64{0x10000} * 0x230;
constexpr u64 ColorStride = 16;
constexpr u64 NoiseBytes = 0x2200;
// The curve atlas, whose last rows the game writes from the CPU.
constexpr vk::Format CurveFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::Extent2D CurveExtent{0x800, 0x400};
constexpr u32 CurveCpuFirstRow = 0x3c0;
constexpr u32 CurveCpuRows = 0x40;
constexpr u64 CurveTexelBytes = 8;
constexpr u64 CurveCpuBytes = u64{CurveExtent.width} * CurveCpuRows * CurveTexelBytes;
// A curve's rows, which are also the height of a block of the atlas. A block holds the columns of
// 0x800 descriptions.
constexpr u32 CurveRows = 0x40;
constexpr u32 CurveBlockColumns = 0x800;
constexpr u32 CurveBlocks = 0x10;
constexpr u64 CurveStagingBytes = u64{CurveExtent.width} * CurveExtent.height * CurveTexelBytes;
constexpr u64 StagingBytes = DescriptionBytes + MaterialBytes + CurveCpuBytes + NoiseBytes;
// Frames the staging buffer stays in use after a copy, and that depth stays unready after a resize.
constexpr u32 FramesInFlight = 3;
constexpr u32 ProgramCount = static_cast<u32>(Scene::ParticleProgram::Count);
constexpr u32 ShapeCount = static_cast<u32>(Scene::ParticleShape::Count);
constexpr u32 FogFirstProgram = static_cast<u32>(Scene::ParticleProgram::Fog);
constexpr u32 FogEndProgram = static_cast<u32>(Scene::ParticleProgram::Distortion);
// The passes with programs, in the game's order: before it copies the scene, those that read the
// copy, and after them. The fog's low path (LW) draws its passes before the fog, the opaque one
// with the opaque draws and the others after TransparentBg; the high path (HW) after the fog.
struct Modes {
    std::span<const u32> before_copy;
    std::span<const u32> of_copy;
    std::span<const u32> after_copy;
};
constexpr std::array<u32, 1> ModesLowOpaque{11};
constexpr std::array<u32, 1> ModesLowBeforeCopy{10};
constexpr std::array<u32, 2> ModesLowOfCopy{12, 13};
constexpr std::array<u32, 2> ModesBeforeCopy{15, 14};
constexpr std::array<u32, 2> ModesOfCopy{16, 17};
constexpr std::array<u32, 2> ModesAfterCopy{1, 0};

Modes ModesOf(Group group) {
    switch (group) {
    case Group::LowOpaque:
        return {.before_copy = ModesLowOpaque};
    case Group::LowTransparent:
        return {.before_copy = ModesLowBeforeCopy, .of_copy = ModesLowOfCopy};
    default:
        return {
            .before_copy = ModesBeforeCopy, .of_copy = ModesOfCopy, .after_copy = ModesAfterCopy};
    }
}

bool Holds(const Modes& modes, u32 mode) {
    const auto holds = [mode](std::span<const u32> list) {
        return std::find(list.begin(), list.end(), mode) != list.end();
    };
    return holds(modes.before_copy) || holds(modes.of_copy) || holds(modes.after_copy);
}
// SelfTranslucent's passes draw sprites, with the programs before the fog's.
constexpr u32 SelfTranslucentFirstMode = 18;
constexpr u32 SelfTranslucentPrograms = FogFirstProgram;
constexpr u32 ShadowPrograms = static_cast<u32>(Scene::ParticleShadowProgram::Count);
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr vk::ImageSubresourceRange DepthRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
// How often the pool is read back to count what is alive, and where a particle holds its life.
constexpr u64 AliveCheckFrames = 600;
constexpr u64 ParticleLifeOffset = 28;

struct DrawPush {
    u64 draws;
    u64 particle;
    u64 emitter;
    u64 effect;
    u64 description;
    u64 material;
    u64 color;
    u64 light;
    u64 fog;
    u64 instance;
    u64 point_lights;
    u64 light_grid;
};

struct UpdatePush {
    u64 emitter_in;
    u64 effect_in;
    u64 owner_in;
    u64 description;
    u64 emitter_out;
};

struct MovePush {
    u64 particle_in;
    u64 emitter_in;
    u64 effect_in;
    u64 description;
    u64 particle_out;
    u64 per_scene;
    u64 noise;
};

struct SpawnPush {
    u64 effect_in;
    u64 description;
    u64 particle_out;
    u64 emitter_out;
};

struct EmitterColorPush {
    u64 emitter_in;
    u64 description;
    u64 color_out;
    u64 per_scene;
};

// The programs without a skin read the members before weights.
struct SamplePush {
    u64 vertices;
    u64 description;
    u64 particle_out;
    u64 emitter_out;
    u64 constants;
    u64 weights;
    u64 palette;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
vk::Format g_scene_format;
vk::Format g_depth_format;
std::atomic<bool> g_ready{false};
std::thread g_builder;

std::array<Pipeline, ShapeCount * ProgramCount> g_draws;
// Pass 1's materials turn blending off: they replace the colour and clear the antialias history's
// mark, with the programs before the fog's.
constexpr u32 OpaqueMode = 1;
constexpr u32 OpaquePrograms = FogFirstProgram;
std::array<Pipeline, ShapeCount * OpaquePrograms> g_opaque;
// Kind 4's effect meshes, blended as the other particles, and replacing the colour in pass 1.
Pipeline g_effect;
Pipeline g_effect_opaque;
std::array<Pipeline, SelfTranslucentPrograms * 2> g_self_translucent;
std::array<Pipeline, ShadowPrograms> g_shadow_pipelines;
std::array<Pipeline, Scene::ParticleSurfacePipelines> g_surface_pipelines;
Pipeline g_update;
Pipeline g_clear;
Pipeline g_move;
Pipeline g_spawn;
Pipeline g_sample;
Pipeline g_sample_skinned;
vk::Sampler g_curve_sampler;
vk::Sampler g_depth_sampler;
vk::DescriptorSetLayout g_compute_layout;
vk::DescriptorSetLayout g_draw_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_compute_curve_set;
vk::DescriptorSet g_compute_depth_set;
// ExtractEmitterColor reads the quarter copy of the albedo that BackupColorBuffer made the frame
// before, then BackupColorBuffer makes this frame's.
Pipeline g_emitter_color;
Pipeline g_color_backup;
// The layout of both copies' sets, which are written when their images are made. Pushed instead,
// each copy reads another image on an RTX 2080 Ti, with validation clean.
vk::DescriptorSetLayout g_backup_layout;
vk::DescriptorSet g_compute_color_set;
// The albedo and the copy.
vk::DescriptorSet g_color_backup_set;
Image g_color_copy;
Buffer g_color_copy_size;
bool g_color_copy_cleared = false;
// BackupCollisionBuffer's copy of the world's depth, at the game's half of its 1920x1080 target,
// which the move program's constants name; and the program that makes it.
constexpr vk::Extent2D CollisionExtent{960, 540};
Pipeline g_collision_backup;
// The scene's depth and the copy.
vk::DescriptorSet g_collision_set;
Image g_collision;
Buffer g_collision_size;
bool g_collision_cleared = false;
vk::DescriptorSet g_draw_set;
// As g_draw_set, with the copy of the depth, for draws while the scene's depth is written.
vk::DescriptorSet g_copy_set;

Image g_curves;
Image g_background;
Image g_depth_copy;
bool g_depth_copy_ready = false;
Buffer g_instances;
Buffer g_staging;
Buffer g_description;
Buffer g_material;
Buffer g_noise;
Buffer g_colors;
std::array<Buffer, 2> g_particles;
std::array<Buffer, 2> g_objects;
u32 g_next = 0;
u32 g_generation = 0;
u64 g_frames = 0;
u64 g_copied_frame = 0;
u64 g_depth_ready_frame = 0;
bool g_noise_copied = false;
bool g_pool_ready = false;
bool g_curves_ready = false;
bool g_fog_ready = false;
struct QueuedCurve {
    u32 destination;
    u32 columns;
    u64 offset;
};
// Curves on their way to the atlas, with their texels one after another, row by row.
std::mutex g_curve_mutex;
std::vector<QueuedCurve> g_curve_queue;
std::vector<u8> g_curve_texels;
Buffer g_curve_staging;
u64 g_curve_copied_frame = 0;
u64 g_curves_copied = 0;
Buffer g_alive_copy;
u64 g_alive_frame = 0;
u32 g_alive_pool = 0;

vk::DescriptorSetLayout CreateLayout(const Device& d,
                                     std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    return d.device
        .createDescriptorSetLayout(
            {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
            nullptr, d.dispatch)
        .value;
}

// Builds the pipelines, some of whose shaders run to thousands of instructions.
void Build() {
    const auto start = std::chrono::steady_clock::now();
    const Device& d = *g_device;
    const std::array compute_sets{g_compute_layout};
    const std::array draw_sets{Textures::SetLayout(), g_draw_layout};
    g_update = CreateComputePipeline(d, GPU_OBJECT_UPDATE_COMP, sizeof(UpdatePush), compute_sets);
    g_clear = CreateComputePipeline(d, GPU_PARTICLE_CLEAR_COMP, sizeof(u64));
    g_move = CreateComputePipeline(d, GPU_PARTICLE_MOVE_COMP, sizeof(MovePush), compute_sets);
    g_spawn = CreateComputePipeline(d, GPU_EMITTER_SPAWN_COMP, sizeof(SpawnPush), compute_sets);
    g_sample = CreateComputePipeline(d, GPU_SAMPLE_MESH_COMP, sizeof(SamplePush));
    g_sample_skinned = CreateComputePipeline(d, GPU_SAMPLE_SKINNED_MESH_COMP, sizeof(SamplePush));
    g_emitter_color =
        CreateComputePipeline(d, EMITTER_COLOR_COMP, sizeof(EmitterColorPush), compute_sets);
    const std::array backup_sets{g_backup_layout};
    g_color_backup = CreateComputePipeline(d, COLOR_BACKUP_COMP, sizeof(u64), backup_sets);
    g_collision_backup = CreateComputePipeline(d, COLLISION_BACKUP_COMP, sizeof(u64), backup_sets);
    using Programs = std::array<std::span<const u32>, ProgramCount>;
    const Programs sprite_pixels{
        SPRITE_PLAIN_FRAG,           SPRITE_PLAIN_SOFT_FRAG,   SPRITE_LIT_FRAG,
        SPRITE_LIT_SOFT_FRAG,        SPRITE_FOG_FRAG,          SPRITE_FOG_SOFT_FRAG,
        SPRITE_FOG_LIT_FRAG,         SPRITE_FOG_LIT_SOFT_FRAG, SPRITE_DISTORTION_FRAG,
        SPRITE_DISTORTION_SOFT_FRAG, SPRITE_BLUR_FRAG,         SPRITE_BLUR_SOFT_FRAG,
    };
    const Programs model_pixels{
        MODEL_PLAIN_FRAG,           MODEL_PLAIN_SOFT_FRAG,   MODEL_LIT_FRAG,
        MODEL_LIT_SOFT_FRAG,        MODEL_FOG_FRAG,          MODEL_FOG_SOFT_FRAG,
        MODEL_FOG_LIT_FRAG,         MODEL_FOG_LIT_SOFT_FRAG, MODEL_DISTORTION_FRAG,
        MODEL_DISTORTION_SOFT_FRAG, MODEL_BLUR_FRAG,         MODEL_BLUR_SOFT_FRAG,
    };
    struct Shape {
        std::span<const u32> vertex;
        vk::PrimitiveTopology topology;
        const Programs& pixels;
    };
    const std::array<Shape, ShapeCount> shapes{{
        {SPRITE_VERT, vk::PrimitiveTopology::eTriangleStrip, sprite_pixels},
        {SPRITE_RIBBON_VERT, vk::PrimitiveTopology::eTriangleStrip, sprite_pixels},
        {MODEL_VERT, vk::PrimitiveTopology::eTriangleList, model_pixels},
    }};
    bool built = g_update.pipeline && g_clear.pipeline && g_move.pipeline && g_spawn.pipeline &&
                 g_sample.pipeline && g_sample_skinned.pipeline && g_emitter_color.pipeline &&
                 g_color_backup.pipeline && g_collision_backup.pipeline;
    for (u32 shape = 0; shape < ShapeCount; shape++) {
        for (u32 program = 0; program < ProgramCount; program++) {
            Pipeline& pipeline = g_draws[shape * ProgramCount + program];
            pipeline =
                CreateGraphicsPipeline(d, {
                                              .vertex = shapes[shape].vertex,
                                              .fragment = shapes[shape].pixels[program],
                                              .color = g_scene_format,
                                              .depth = g_depth_format,
                                              .topology = shapes[shape].topology,
                                              .depth_test = true,
                                              .depth_write = false,
                                              .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                              .premultiplied_blend = true,
                                              .clear_target_alpha = true,
                                              .dynamic_depth_and_cull = true,
                                              .push_constant_bytes = sizeof(DrawPush),
                                              .set_layouts = draw_sets,
                                          });
            built = built && pipeline.pipeline;
        }
    }
    constexpr vk::PipelineColorBlendAttachmentState Replace{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eOne,
        .dstColorBlendFactor = vk::BlendFactor::eZero,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    for (u32 shape = 0; shape < ShapeCount; shape++) {
        for (u32 program = 0; program < OpaquePrograms; program++) {
            Pipeline& pipeline = g_opaque[shape * OpaquePrograms + program];
            pipeline =
                CreateGraphicsPipeline(d, {
                                              .vertex = shapes[shape].vertex,
                                              .fragment = shapes[shape].pixels[program],
                                              .color = g_scene_format,
                                              .depth = g_depth_format,
                                              .topology = shapes[shape].topology,
                                              .depth_test = true,
                                              .depth_write = false,
                                              .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                              .blend = &Replace,
                                              .dynamic_depth_and_cull = true,
                                              .push_constant_bytes = sizeof(DrawPush),
                                              .set_layouts = draw_sets,
                                          });
            built = built && pipeline.pipeline;
        }
    }
    const GraphicsPipelineInfo effect{
        .vertex = PARTICLE_EFFECT_VERT,
        .fragment = PARTICLE_EFFECT_FRAG,
        .color = g_scene_format,
        .depth = g_depth_format,
        .topology = vk::PrimitiveTopology::eTriangleList,
        .depth_test = true,
        .depth_write = false,
        .depth_compare = vk::CompareOp::eGreaterOrEqual,
        .premultiplied_blend = true,
        .clear_target_alpha = true,
        .dynamic_depth_and_cull = true,
        .push_constant_bytes = sizeof(DrawPush),
        .set_layouts = draw_sets,
    };
    GraphicsPipelineInfo effect_opaque = effect;
    effect_opaque.premultiplied_blend = false;
    effect_opaque.clear_target_alpha = false;
    effect_opaque.blend = &Replace;
    g_effect = CreateGraphicsPipeline(d, effect);
    g_effect_opaque = CreateGraphicsPipeline(d, effect_opaque);
    built = built && g_effect.pipeline && g_effect_opaque.pipeline;
    for (u32 program = 0; program < SelfTranslucentPrograms; program++) {
        for (u32 tested = 0; tested < 2; tested++) {
            Pipeline& inside = g_self_translucent[program * 2 + tested];
            inside =
                CreateGraphicsPipeline(d, {
                                              .vertex = SPRITE_VERT,
                                              .fragment = sprite_pixels[program],
                                              .color = SelfTranslucent::ColorFormat,
                                              .depth = SelfTranslucent::DepthFormat,
                                              .topology = vk::PrimitiveTopology::eTriangleStrip,
                                              .depth_test = tested != 0,
                                              .depth_write = false,
                                              .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                              .stencil_equal = true,
                                              .premultiplied_blend = true,
                                              .push_constant_bytes = sizeof(DrawPush),
                                              .set_layouts = draw_sets,
                                          });
            built = built && inside.pipeline;
        }
    }
    struct ProgramPair {
        std::span<const u32> vertex;
        std::span<const u32> fragment;
        vk::PrimitiveTopology topology;
    };
    const std::array<ProgramPair, ShadowPrograms> shadow_programs{{
        {SPRITE_SHADOW_VERT, SPRITE_SHADOW_FRAG, vk::PrimitiveTopology::eTriangleStrip},
        {SPRITE_SHADOW_VERT, SPRITE_SHADOW_SOFT_FRAG, vk::PrimitiveTopology::eTriangleStrip},
        {MODEL_SHADOW_VERT, {}, vk::PrimitiveTopology::eTriangleList},
        {SPRITE_RIBBON_SHADOW_VERT, SPRITE_SHADOW_FRAG, vk::PrimitiveTopology::eTriangleStrip},
        {SPRITE_RIBBON_SHADOW_VERT, SPRITE_SHADOW_SOFT_FRAG, vk::PrimitiveTopology::eTriangleStrip},
        {PARTICLE_SURFACE_SHADOW_VERT, PARTICLE_MESH_SHADOW_FRAG,
         vk::PrimitiveTopology::eTriangleList},
        {PARTICLE_GEM_SHADOW_VERT, PARTICLE_MESH_SHADOW_FRAG, vk::PrimitiveTopology::eTriangleList},
        {PARTICLE_EFFECT_SHADOW_VERT, PARTICLE_EFFECT_SHADOW_FRAG,
         vk::PrimitiveTopology::eTriangleList},
    }};
    for (u32 i = 0; i < ShadowPrograms; i++) {
        g_shadow_pipelines[i] =
            CreateGraphicsPipeline(d, {
                                          .vertex = shadow_programs[i].vertex,
                                          .fragment = shadow_programs[i].fragment,
                                          .depth = Shadows::DepthFormat,
                                          .topology = shadow_programs[i].topology,
                                          .depth_test = true,
                                          .depth_compare = vk::CompareOp::eLessOrEqual,
                                          .depth_bias = true,
                                          .dynamic_depth_and_cull = true,
                                          .push_constant_bytes = sizeof(DrawPush),
                                          .set_layouts = draw_sets,
                                      });
        built = built && g_shadow_pipelines[i].pipeline;
    }
    const std::array<ProgramPair, Scene::ParticleSurfaceVariants> surface_programs{{
        {PARTICLE_SURFACE_VERT, PARTICLE_SURFACE_FRAG, vk::PrimitiveTopology::eTriangleList},
        {PARTICLE_SURFACE_TANGENT_VERT, PARTICLE_SURFACE_TANGENT_FRAG,
         vk::PrimitiveTopology::eTriangleList},
        {PARTICLE_SURFACE_COLOR_VERT, PARTICLE_SURFACE_COLOR_FRAG,
         vk::PrimitiveTopology::eTriangleList},
        {PARTICLE_SURFACE_TANGENT_COLOR_VERT, PARTICLE_SURFACE_TANGENT_COLOR_FRAG,
         vk::PrimitiveTopology::eTriangleList},
    }};
    for (u32 i = 0; i < Scene::ParticleSurfaceVariants; i++) {
        g_surface_pipelines[i] =
            CreateGraphicsPipeline(d, {
                                          .vertex = surface_programs[i].vertex,
                                          .fragment = surface_programs[i].fragment,
                                          .colors = World::SurfaceFormats,
                                          .depth = g_depth_format,
                                          .topology = surface_programs[i].topology,
                                          .depth_test = true,
                                          .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                          .dynamic_depth_and_cull = true,
                                          .push_constant_bytes = sizeof(DrawPush),
                                          .set_layouts = draw_sets,
                                      });
        built = built && g_surface_pipelines[i].pipeline;
    }
    const std::array<std::span<const u32>, 2> sprite_gbuffer{SPRITE_GBUFFER_FRAG,
                                                             SPRITE_GBUFFER_SOFT_FRAG};
    const std::array<std::span<const u32>, 2> model_gbuffer{MODEL_GBUFFER_FRAG,
                                                            MODEL_GBUFFER_SOFT_FRAG};
    for (u32 shape = 0; shape < ShapeCount; shape++) {
        const bool model = shape == static_cast<u32>(Scene::ParticleShape::Model);
        for (u32 soft = 0; soft < 2; soft++) {
            Pipeline& pipeline =
                g_surface_pipelines[Scene::ParticleSurfaceVariants + shape * 2 + soft];
            pipeline = CreateGraphicsPipeline(
                d, {
                       .vertex = shapes[shape].vertex,
                       .fragment = model ? model_gbuffer[soft] : sprite_gbuffer[soft],
                       .colors = World::SurfaceFormats,
                       .depth = g_depth_format,
                       .topology = shapes[shape].topology,
                       .depth_test = true,
                       .depth_compare = vk::CompareOp::eGreaterOrEqual,
                       .dynamic_depth_and_cull = true,
                       .push_constant_bytes = sizeof(DrawPush),
                       .set_layouts = draw_sets,
                   });
            built = built && pipeline.pipeline;
        }
    }
    Pipeline& gem = g_surface_pipelines[Scene::ParticleSurfaceGem];
    gem = CreateGraphicsPipeline(d, {
                                        .vertex = PARTICLE_GEM_VERT,
                                        .fragment = PARTICLE_GEM_FRAG,
                                        .colors = World::SurfaceFormats,
                                        .depth = g_depth_format,
                                        .topology = vk::PrimitiveTopology::eTriangleList,
                                        .depth_test = true,
                                        .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                        .dynamic_depth_and_cull = true,
                                        .push_constant_bytes = sizeof(DrawPush),
                                        .set_layouts = draw_sets,
                                    });
    built = built && gem.pipeline;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!built) {
        LOG_ERROR(Render_Vulkan, "GR2port particles: a pipeline failed, after {} ms", ms);
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2port particles: pipelines built in {} ms", ms);
    SavePipelineCache(d);
    g_ready.store(true, std::memory_order_release);
}

void WriteSampledImage(vk::DescriptorSet set, u32 binding, vk::ImageView view,
                       vk::ImageLayout layout) {
    const vk::DescriptorImageInfo info{.imageView = view, .imageLayout = layout};
    const vk::WriteDescriptorSet write{
        .dstSet = set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
}

void WriteStorageImage(vk::DescriptorSet set, u32 binding, vk::ImageView view) {
    const vk::DescriptorImageInfo info{.imageView = view, .imageLayout = vk::ImageLayout::eGeneral};
    const vk::WriteDescriptorSet write{
        .dstSet = set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eStorageImage,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
}

Buffer CreateStorage(u64 bytes) {
    return g_memory->CreateBuffer(bytes,
                                  vk::BufferUsageFlagBits::eStorageBuffer |
                                      vk::BufferUsageFlagBits::eTransferDst |
                                      vk::BufferUsageFlagBits::eTransferSrc,
                                  Placement::Device);
}

// Makes the pool at the sizes the game chose, zeroed as the game's are.
bool CreatePool(vk::CommandBuffer cmd, const Scene::ParticleHeader& frame) {
    const auto& dl = g_device->dispatch;
    for (u32 i = 0; i < 2; i++) {
        g_particles[i] = CreateStorage(frame.particle_bytes);
        g_objects[i] = CreateStorage(frame.object_bytes);
        if (!g_particles[i].buffer || !g_objects[i].buffer) {
            return false;
        }
        cmd.fillBuffer(g_particles[i].buffer, 0, VK_WHOLE_SIZE, 0, dl);
        cmd.fillBuffer(g_objects[i].buffer, 0, VK_WHOLE_SIZE, 0, dl);
    }
    const u64 objects = frame.object_bytes / Scene::ParticleObjectBytes;
    g_colors = CreateStorage(objects * ColorStride);
    if (!g_colors.buffer) {
        return false;
    }
    // Emitters stay white until the first frame takes their colour from the albedo.
    const float one = 1.0f;
    u32 bits;
    std::memcpy(&bits, &one, sizeof(bits));
    cmd.fillBuffer(g_colors.buffer, 0, VK_WHOLE_SIZE, bits, dl);
    g_alive_copy = g_memory->CreateBuffer(
        frame.particle_bytes, vk::BufferUsageFlagBits::eTransferDst, Placement::Readback);
    if (!g_alive_copy.buffer) {
        return false;
    }
    // Ribbons can read their particles through a list, here in the pool's order.
    const u32 capacity = static_cast<u32>(frame.particle_bytes / Scene::ParticleBytes);
    g_instances = g_memory->CreateBuffer(
        capacity * sizeof(u32), vk::BufferUsageFlagBits::eStorageBuffer, Placement::Upload);
    if (!g_instances.buffer) {
        return false;
    }
    for (u32 i = 0; i < capacity; i++) {
        std::memcpy(g_instances.mapped + u64{i} * sizeof(u32), &i, sizeof(u32));
    }
    LOG_INFO(Render_Vulkan, "GR2port particles: pool {} bytes for {}, objects {} bytes for {}",
             frame.particle_bytes, frame.particles, frame.object_bytes, frame.objects);
    return true;
}

// Now and then copies the pool the simulation has just written, and later counts what is alive.
void CheckAlive(vk::CommandBuffer cmd, const Scene::ParticleHeader& frame, const Buffer& pool) {
    if (g_alive_frame != 0) {
        if (g_frames < g_alive_frame + FramesInFlight) {
            return;
        }
        u32 alive = 0;
        for (u32 i = 0; i < g_alive_pool; i++) {
            float life;
            std::memcpy(&life, g_alive_copy.mapped + i * Scene::ParticleBytes + ParticleLifeOffset,
                        sizeof(life));
            alive += life > 0.0f ? 1 : 0;
        }
        LOG_INFO(Render_Vulkan, "GR2port particles: {} alive of a pool of {}", alive, g_alive_pool);
        g_alive_frame = 0;
    }
    if (g_frames % AliveCheckFrames != 0) {
        return;
    }
    g_alive_frame = g_frames;
    g_alive_pool = frame.particles;
    const auto& dl = g_device->dispatch;
    cmd.copyBuffer(pool.buffer, g_alive_copy.buffer,
                   vk::BufferCopy{.size = u64{g_alive_pool} * Scene::ParticleBytes}, dl);
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &to_host}, dl);
}

// Copies what the game changed in its library, when the staging buffer is free.
void CopyLibrary(vk::CommandBuffer cmd, const Scene::ParticleHeader& frame) {
    const auto& dl = g_device->dispatch;
    if (frame.generation == g_generation || g_frames < g_copied_frame + FramesInFlight ||
        frame.description_source == 0 || frame.material_source == 0) {
        return;
    }
    g_generation = frame.generation;
    g_copied_frame = g_frames;
    std::memcpy(g_staging.mapped, reinterpret_cast<const void*>(frame.description_source),
                DescriptionBytes);
    std::memcpy(g_staging.mapped + DescriptionBytes,
                reinterpret_cast<const void*>(frame.material_source), MaterialBytes);
    cmd.copyBuffer(g_staging.buffer, g_description.buffer, vk::BufferCopy{.size = DescriptionBytes},
                   dl);
    cmd.copyBuffer(g_staging.buffer, g_material.buffer,
                   vk::BufferCopy{.srcOffset = DescriptionBytes, .size = MaterialBytes}, dl);
    if (frame.curve_source != 0) {
        const u64 at = DescriptionBytes + MaterialBytes;
        std::memcpy(g_staging.mapped + at, reinterpret_cast<const void*>(frame.curve_source),
                    CurveCpuBytes);
        const vk::BufferImageCopy region{
            .bufferOffset = at,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageOffset = {0, static_cast<s32>(CurveCpuFirstRow), 0},
            .imageExtent = {CurveExtent.width, CurveCpuRows, 1},
        };
        cmd.copyBufferToImage(g_staging.buffer, g_curves.image, vk::ImageLayout::eGeneral, region,
                              dl);
    }
    LOG_INFO(Render_Vulkan, "GR2port particles: library copied, generation {}", g_generation);
}

// Copies the descriptions the game rewrites each frame, which this packet holds.
void CopyLiveDescriptions(vk::CommandBuffer cmd, const Scene::ParticleHeader& frame,
                          const u8* packet, vk::Buffer buffer, u64 offset) {
    if (frame.live_descriptions == 0) {
        return;
    }
    std::array<u32, Scene::MaxParticleLiveDescriptions> indices;
    std::memcpy(indices.data(), packet + Scene::ParticleLiveIndicesOffset,
                frame.live_descriptions * sizeof(u32));
    std::array<vk::BufferCopy, Scene::MaxParticleLiveDescriptions> regions;
    for (u32 i = 0; i < frame.live_descriptions; i++) {
        regions[i] = {
            .srcOffset = offset + Scene::ParticleLiveOffset + i * Scene::ParticleDescriptionBytes,
            .dstOffset = indices[i] * Scene::ParticleDescriptionBytes,
            .size = Scene::ParticleDescriptionBytes,
        };
    }
    cmd.copyBuffer(buffer, g_description.buffer,
                   vk::ArrayProxy<const vk::BufferCopy>(frame.live_descriptions, regions.data()),
                   g_device->dispatch);
}

// Copies the curves the game has made into the atlas, as its bake program does.
void CopyCurves(vk::CommandBuffer cmd) {
    if (g_frames < g_curve_copied_frame + FramesInFlight) {
        return;
    }
    std::vector<QueuedCurve> curves;
    std::vector<u8> texels;
    {
        std::scoped_lock lock{g_curve_mutex};
        curves.swap(g_curve_queue);
        texels.swap(g_curve_texels);
    }
    if (curves.empty()) {
        return;
    }
    if (texels.size() > CurveStagingBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port particles: {} bytes of curves do not fit, {} curves lost",
                  texels.size(), curves.size());
        return;
    }
    g_curve_copied_frame = g_frames;
    std::memcpy(g_curve_staging.mapped, texels.data(), texels.size());
    std::vector<vk::BufferImageCopy> regions;
    for (const QueuedCurve& curve : curves) {
        for (u32 column = 0; column < curve.columns;) {
            const u32 at = curve.destination + column;
            const u32 x = at % CurveBlockColumns;
            const u32 block = at / CurveBlockColumns;
            const u32 columns = std::min(curve.columns - column, CurveBlockColumns - x);
            if (block < CurveBlocks) {
                regions.push_back({
                    .bufferOffset = curve.offset + column * CurveTexelBytes,
                    .bufferRowLength = curve.columns,
                    .bufferImageHeight = CurveRows,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageOffset = {static_cast<s32>(x), static_cast<s32>(block * CurveRows), 0},
                    .imageExtent = {columns, CurveRows, 1},
                });
            }
            column += columns;
        }
    }
    cmd.copyBufferToImage(g_curve_staging.buffer, g_curves.image, vk::ImageLayout::eGeneral,
                          regions, g_device->dispatch);
    g_curves_copied += curves.size();
    LOG_INFO(Render_Vulkan, "GR2port particles: {} curves copied, {} in all", curves.size(),
             g_curves_copied);
}

// Orders the next work after everything earlier: the pool passes between passes this way.
void FullBarrier(vk::CommandBuffer cmd) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier},
                         g_device->dispatch);
}

u32 Groups(u32 threads) {
    return (threads + GroupThreads - 1) / GroupThreads;
}

// Places the particles of the frame's meshes that do so, each after the last: they share the pool.
void RecordSamples(vk::CommandBuffer cmd, const Scene::ParticleHeader& frame, const u8* packet,
                   u64 address, u32 next) {
    const auto& dl = g_device->dispatch;
    for (u32 i = 0; i < frame.samples; i++) {
        const u64 at = Scene::ParticleSamplesOffset + u64{i} * sizeof(Scene::ParticleSample);
        Scene::ParticleSample sample;
        std::memcpy(&sample, packet + at, sizeof(sample));
        const SamplePush push{
            .vertices = sample.vertices,
            .description = g_description.address,
            .particle_out = g_particles[next].address,
            .emitter_out = g_objects[next].address,
            .constants = address + at,
            .weights = sample.weights,
            .palette = address + sample.palette_offset,
        };
        const Pipeline& pipeline = sample.weights != 0 ? g_sample_skinned : g_sample;
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                          &push, dl);
        cmd.dispatch(1, 1, 1, dl);
        FullBarrier(cmd);
    }
}

DrawPush MakePush(u64 address) {
    const u32 current = g_next;
    return {
        .draws = address + Scene::ParticleDrawsOffset,
        .particle = g_particles[current].address,
        .emitter = g_objects[current].address,
        .effect = address + Scene::ParticleEffectsOffset,
        .description = g_description.address,
        .material = g_material.address,
        .color = g_colors.address,
        .light = address + Scene::ParticleLightOffset,
        .fog = address + Scene::ParticleFogOffset,
        .instance = g_instances.address,
        .point_lights = address + Scene::PointLightsOffset,
        .light_grid = PointLights::GridAddress(),
    };
}

void SetRunState(vk::CommandBuffer cmd, const Scene::ParticleRun& run, bool can_write) {
    const auto& dl = g_device->dispatch;
    cmd.setDepthTestEnable(run.tested != 0, dl);
    cmd.setDepthWriteEnable(run.write != 0 && can_write, dl);
    cmd.setCullMode(run.cull == 1   ? vk::CullModeFlagBits::eFront
                    : run.cull == 2 ? vk::CullModeFlagBits::eBack
                                    : vk::CullModeFlagBits::eNone,
                    dl);
}

void DrawRun(vk::CommandBuffer cmd, const Scene::ParticleRun& run, vk::Buffer buffer, u64 offset) {
    const auto& dl = g_device->dispatch;
    if (run.indexed != 0) {
        cmd.drawIndexedIndirect(buffer,
                                offset + Scene::ParticleIndexedCommandsOffset +
                                    run.first * sizeof(Scene::Command),
                                run.count, sizeof(Scene::Command), dl);
    } else {
        cmd.drawIndirect(buffer,
                         offset + Scene::ParticleCommandsOffset +
                             run.first * sizeof(Scene::VertexCommand),
                         run.count, sizeof(Scene::VertexCommand), dl);
    }
}

// Draws a packet's runs of one mode that have their own pipelines, inside a rendering.
void DrawOwnRuns(vk::CommandBuffer cmd, const Scene::ParticleHeader& frame, const u8* packet,
                 u64 address, vk::Buffer buffer, u64 offset, u32 mode,
                 std::span<const Pipeline> pipelines) {
    const auto& dl = g_device->dispatch;
    std::array<Scene::ParticleRun, Scene::MaxParticleDraws> runs;
    std::memcpy(runs.data(), packet + Scene::ParticleRunsOffset,
                frame.runs * sizeof(Scene::ParticleRun));
    const DrawPush push = MakePush(address);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    u32 bound = ~0u;
    for (u32 i = 0; i < frame.runs; i++) {
        const Scene::ParticleRun& run = runs[i];
        if (run.mode != mode || run.pipeline >= pipelines.size()) {
            continue;
        }
        if (run.pipeline != bound) {
            bound = run.pipeline;
            const Pipeline& pipeline = pipelines[run.pipeline];
            const std::array sets{Textures::Set(), g_copy_set};
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {},
                                   dl);
            cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                              sizeof(push), &push, dl);
        }
        SetRunState(cmd, run, true);
        DrawRun(cmd, run, buffer, offset);
    }
}

// Clears the copy of the depth, which the shaders may name before any frame has copied it.
void PrepareDepthCopy(vk::CommandBuffer cmd) {
    const auto& dl = g_device->dispatch;
    const vk::ImageMemoryBarrier2 to_clear{
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = g_depth_copy.image,
        .subresourceRange = DepthRange,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear}, dl);
    cmd.clearDepthStencilImage(g_depth_copy.image, vk::ImageLayout::eTransferDstOptimal,
                               vk::ClearDepthStencilValue{0.0f, 0}, DepthRange, dl);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
        .image = g_depth_copy.image,
        .subresourceRange = DepthRange,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read}, dl);
}

// Clears the collision copy to the far depth and leaves it in its only layout.
void ClearCollision(vk::CommandBuffer cmd) {
    const auto& dl = g_device->dispatch;
    g_collision_cleared = true;
    constexpr vk::ImageSubresourceRange Range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 layout{
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = g_collision.image,
        .subresourceRange = Range,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &layout}, dl);
    cmd.clearColorImage(g_collision.image, vk::ImageLayout::eGeneral,
                        vk::ClearColorValue{.float32 = std::array<float, 4>{0, 0, 0, 0}}, Range,
                        dl);
    FullBarrier(cmd);
}

// Copies the scene's depth for the programs to read while draws write it, as the game's copy of
// its depth lets them, and leaves the depth an attachment.
void CopyDepth(vk::CommandBuffer cmd, const Image& depth, vk::Extent2D extent) {
    const auto& dl = g_device->dispatch;
    constexpr auto Copy = vk::PipelineStageFlagBits2::eCopy;
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    constexpr auto ReadOnly = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    constexpr auto Source = vk::ImageLayout::eTransferSrcOptimal;
    constexpr auto Target = vk::ImageLayout::eTransferDstOptimal;
    const std::array<vk::ImageMemoryBarrier2, 2> before{{
        {.srcStageMask = Fragment | Tests,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
         .oldLayout = ReadOnly,
         .newLayout = Source,
         .image = depth.image,
         .subresourceRange = DepthRange},
        {.srcStageMask = vk::PipelineStageFlagBits2::eVertexShader | Fragment,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .oldLayout = vk::ImageLayout::eUndefined,
         .newLayout = Target,
         .image = g_depth_copy.image,
         .subresourceRange = DepthRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(before.size()),
                          .pImageMemoryBarriers = before.data()},
                         dl);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .extent = vk::Extent3D{extent.width, extent.height, 1},
    };
    cmd.copyImage(depth.image, Source, g_depth_copy.image, Target, region, dl);
    const std::array<vk::ImageMemoryBarrier2, 2> after{{
        {.srcStageMask = Copy,
         .dstStageMask = Tests,
         .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
         .oldLayout = Source,
         .newLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
         .image = depth.image,
         .subresourceRange = DepthRange},
        {.srcStageMask = Copy,
         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader | Fragment,
         .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
         .oldLayout = Target,
         .newLayout = ReadOnly,
         .image = g_depth_copy.image,
         .subresourceRange = DepthRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(after.size()),
                          .pImageMemoryBarriers = after.data()},
                         dl);
}

// Copies the scene so far for the passes that read it, as the game's BackupColorBuffer3 does.
void CopyScene(vk::CommandBuffer cmd, vk::Extent2D extent) {
    const auto& dl = g_device->dispatch;
    constexpr auto Output = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr auto Copy = vk::PipelineStageFlagBits2::eCopy;
    constexpr auto Attachment = vk::ImageLayout::eColorAttachmentOptimal;
    constexpr auto Source = vk::ImageLayout::eTransferSrcOptimal;
    constexpr auto Target = vk::ImageLayout::eTransferDstOptimal;
    const vk::Image scene = Look::SceneImage();
    // The last frame's draws sampled the copy.
    const std::array<vk::ImageMemoryBarrier2, 2> before{{
        {.srcStageMask = Output,
         .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
         .oldLayout = Attachment,
         .newLayout = Source,
         .image = scene,
         .subresourceRange = ColorRange},
        {.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .oldLayout = vk::ImageLayout::eUndefined,
         .newLayout = Target,
         .image = g_background.image,
         .subresourceRange = ColorRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(before.size()),
                          .pImageMemoryBarriers = before.data()},
                         dl);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = vk::Extent3D{extent.width, extent.height, 1},
    };
    cmd.copyImage(scene, Source, g_background.image, Target, region, dl);
    const std::array<vk::ImageMemoryBarrier2, 2> after{{
        {.srcStageMask = Copy,
         .dstStageMask = Output,
         .dstAccessMask =
             vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
         .oldLayout = Source,
         .newLayout = Attachment,
         .image = scene,
         .subresourceRange = ColorRange},
        {.srcStageMask = Copy,
         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
         .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
         .oldLayout = Target,
         .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
         .image = g_background.image,
         .subresourceRange = ColorRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(after.size()),
                          .pImageMemoryBarriers = after.data()},
                         dl);
}

} // namespace

void QueueCurve(u32 destination, u32 columns, VAddr texels, u32 pitch) {
    const u64 row_bytes = columns * CurveTexelBytes;
    std::scoped_lock lock{g_curve_mutex};
    const u64 offset = g_curve_texels.size();
    g_curve_queue.push_back({destination, columns, offset});
    g_curve_texels.resize(offset + row_bytes * CurveRows);
    for (u32 row = 0; row < CurveRows; row++) {
        std::memcpy(g_curve_texels.data() + offset + row * row_bytes,
                    reinterpret_cast<const void*>(texels + u64{row} * pitch * CurveTexelBytes),
                    row_bytes);
    }
}

bool Start(const Device& device, Memory& memory, vk::Format scene_format, vk::Format depth_format) {
    g_device = &device;
    g_memory = &memory;
    g_scene_format = scene_format;
    g_depth_format = depth_format;
    const auto& dl = device.dispatch;
    const vk::SamplerCreateInfo curve_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_curve_sampler = device.device.createSampler(curve_info, nullptr, dl).value;
    vk::SamplerCreateInfo depth_info = curve_info;
    depth_info.magFilter = vk::Filter::eNearest;
    depth_info.minFilter = vk::Filter::eNearest;
    g_depth_sampler = device.device.createSampler(depth_info, nullptr, dl).value;
    constexpr auto Sampled = vk::DescriptorType::eSampledImage;
    constexpr auto Sampler = vk::DescriptorType::eSampler;
    constexpr auto Compute = vk::ShaderStageFlagBits::eCompute;
    constexpr vk::ShaderStageFlags Drawn =
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 2> compute_bindings{{
        {0, Sampled, 1, Compute},
        {1, Sampler, 1, Compute, &g_curve_sampler},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 6> draw_bindings{{
        {0, Sampled, 1, Drawn},
        {1, Sampler, 1, Drawn, &g_curve_sampler},
        {2, Sampled, 1, Drawn},
        {3, Sampler, 1, Drawn, &g_depth_sampler},
        {4, Sampled, 1, Drawn},
        {5, Sampled, 1, Drawn},
    }};
    g_compute_layout = CreateLayout(device, compute_bindings);
    g_draw_layout = CreateLayout(device, draw_bindings);
    const std::array<vk::DescriptorSetLayoutBinding, 3> backup_bindings{{
        {0, Sampled, 1, Compute},
        {1, Sampler, 1, Compute, &g_depth_sampler},
        {2, vk::DescriptorType::eStorageImage, 1, Compute},
    }};
    g_backup_layout = CreateLayout(device, backup_bindings);
    const std::array<vk::DescriptorPoolSize, 3> sizes{{
        {Sampled, 13},
        {Sampler, 9},
        {vk::DescriptorType::eStorageImage, 2},
    }};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 7,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_curve_sampler || !g_depth_sampler || !g_compute_layout || !g_draw_layout ||
        !g_backup_layout || !g_pool) {
        return false;
    }
    const std::array layouts{g_compute_layout, g_compute_layout, g_draw_layout,  g_draw_layout,
                             g_compute_layout, g_backup_layout,  g_backup_layout};
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool,
         .descriptorSetCount = static_cast<u32>(layouts.size()),
         .pSetLayouts = layouts.data()},
        dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_compute_curve_set = sets[0];
    g_compute_depth_set = sets[1];
    g_draw_set = sets[2];
    g_copy_set = sets[3];
    g_compute_color_set = sets[4];
    g_collision_set = sets[5];
    g_color_backup_set = sets[6];

    g_curves =
        memory.CreateImage(CurveFormat, CurveExtent,
                           vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                           vk::ImageAspectFlagBits::eColor);
    g_curve_staging = memory.CreateBuffer(CurveStagingBytes, vk::BufferUsageFlagBits::eTransferSrc,
                                          Placement::Staging);
    g_staging = memory.CreateBuffer(StagingBytes, vk::BufferUsageFlagBits::eTransferSrc,
                                    Placement::Staging);
    g_description = CreateStorage(DescriptionBytes);
    g_material = CreateStorage(MaterialBytes);
    g_noise = CreateStorage(NoiseBytes);
    if (!g_curves.image || !g_staging.buffer || !g_curve_staging.buffer || !g_description.buffer ||
        !g_material.buffer || !g_noise.buffer) {
        return false;
    }
    WriteSampledImage(g_compute_curve_set, 0, g_curves.view, vk::ImageLayout::eGeneral);
    WriteSampledImage(g_draw_set, 0, g_curves.view, vk::ImageLayout::eGeneral);
    WriteSampledImage(g_copy_set, 0, g_curves.view, vk::ImageLayout::eGeneral);
    g_builder = std::thread{Build};
    g_builder.detach();
    return true;
}

bool Resize(vk::ImageView depth, vk::ImageView albedo, vk::Extent2D extent) {
    g_memory->Destroy(g_background);
    g_background = g_memory->CreateImage(g_scene_format, extent,
                                         vk::ImageUsageFlagBits::eSampled |
                                             vk::ImageUsageFlagBits::eTransferDst,
                                         vk::ImageAspectFlagBits::eColor);
    g_memory->Destroy(g_depth_copy);
    g_depth_copy = g_memory->CreateImage(g_depth_format, extent,
                                         vk::ImageUsageFlagBits::eSampled |
                                             vk::ImageUsageFlagBits::eTransferDst |
                                             vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                         vk::ImageAspectFlagBits::eDepth);
    if (!g_background.image || !g_depth_copy.image) {
        return false;
    }
    g_depth_copy_ready = false;
    WriteSampledImage(g_draw_set, 5, g_background.view, vk::ImageLayout::eShaderReadOnlyOptimal);
    WriteSampledImage(g_copy_set, 5, g_background.view, vk::ImageLayout::eShaderReadOnlyOptimal);
    WriteSampledImage(g_copy_set, 2, g_depth_copy.view,
                      vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    WriteSampledImage(g_draw_set, 2, depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    const vk::ImageView fog = Fog::ScatteredView();
    if (fog) {
        WriteSampledImage(g_draw_set, 4, fog, vk::ImageLayout::eGeneral);
        WriteSampledImage(g_copy_set, 4, fog, vk::ImageLayout::eGeneral);
        g_fog_ready = true;
    }
    g_depth_ready_frame = g_frames + FramesInFlight;
    // A quarter of the render size, as the game's copy, which its extraction's constants name.
    constexpr u32 CopyScale = 4;
    const vk::Extent2D copy_extent{extent.width / CopyScale, extent.height / CopyScale};
    g_memory->Destroy(g_color_copy);
    g_color_copy =
        g_memory->CreateImage(vk::Format::eR16G16B16A16Sfloat, copy_extent,
                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                                  vk::ImageUsageFlagBits::eTransferDst,
                              vk::ImageAspectFlagBits::eColor);
    if (!g_color_copy_size.buffer) {
        g_color_copy_size = g_memory->CreateBuffer(
            4 * sizeof(float), vk::BufferUsageFlagBits::eStorageBuffer, Placement::Upload);
    }
    if (!g_color_copy.image || g_color_copy_size.mapped == nullptr) {
        return false;
    }
    // u_f4Size: the copy's size, then the albedo's pixels to each of the copy's.
    const std::array<float, 4> size{static_cast<float>(copy_extent.width),
                                    static_cast<float>(copy_extent.height),
                                    static_cast<float>(extent.width) / copy_extent.width,
                                    static_cast<float>(extent.height) / copy_extent.height};
    std::memcpy(g_color_copy_size.mapped, size.data(), sizeof(size));
    g_color_copy_cleared = false;
    WriteSampledImage(g_compute_color_set, 0, g_color_copy.view, vk::ImageLayout::eGeneral);
    WriteSampledImage(g_color_backup_set, 0, albedo, vk::ImageLayout::eShaderReadOnlyOptimal);
    WriteStorageImage(g_color_backup_set, 2, g_color_copy.view);

    g_memory->Destroy(g_collision);
    g_collision =
        g_memory->CreateImage(vk::Format::eR32Sfloat, CollisionExtent,
                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                                  vk::ImageUsageFlagBits::eTransferDst,
                              vk::ImageAspectFlagBits::eColor);
    if (!g_collision_size.buffer) {
        g_collision_size = g_memory->CreateBuffer(
            4 * sizeof(float), vk::BufferUsageFlagBits::eStorageBuffer, Placement::Upload);
    }
    if (!g_collision.image || g_collision_size.mapped == nullptr) {
        return false;
    }
    // u_f4Size: the copy's size, then the depth's pixels to each of the copy's.
    const std::array<float, 4> collision_size{
        static_cast<float>(CollisionExtent.width), static_cast<float>(CollisionExtent.height),
        static_cast<float>(extent.width) / CollisionExtent.width,
        static_cast<float>(extent.height) / CollisionExtent.height};
    std::memcpy(g_collision_size.mapped, collision_size.data(), sizeof(collision_size));
    g_collision_cleared = false;
    WriteSampledImage(g_compute_depth_set, 0, g_collision.view, vk::ImageLayout::eGeneral);
    WriteSampledImage(g_collision_set, 0, depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    WriteStorageImage(g_collision_set, 2, g_collision.view);
    return true;
}

bool DepthReady() {
    return g_frames >= g_depth_ready_frame;
}

void RecordSimulation(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                      u64 address, vk::Buffer buffer, u64 offset) {
    g_frames++;
    const Scene::ParticleHeader& frame = header.particles;
    if (!g_ready.load(std::memory_order_acquire) || frame.refused != 0 || frame.objects == 0 ||
        frame.particle_bytes == 0 || frame.object_bytes == 0) {
        return;
    }
    const auto& dl = g_device->dispatch;
    if (!g_depth_copy_ready) {
        g_depth_copy_ready = true;
        PrepareDepthCopy(cmd);
    }
    if (!g_collision_cleared && g_collision.image) {
        // Until the first copy, nothing is there to collide with.
        ClearCollision(cmd);
    }
    if (!g_curves_ready) {
        // The first frame leaves the curve atlas in its only layout.
        g_curves_ready = true;
        const vk::ImageMemoryBarrier2 layout{
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = g_curves.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &layout}, dl);
        cmd.clearColorImage(g_curves.image, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{.float32 = std::array<float, 4>{}},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                            dl);
        FullBarrier(cmd);
    }
    if (!g_pool_ready) {
        if (!CreatePool(cmd, frame)) {
            LOG_ERROR(Render_Vulkan, "GR2port particles: no memory for the pool");
            return;
        }
        g_pool_ready = true;
    }
    if (!g_noise_copied) {
        // Past the library's part of the staging buffer: it is read once.
        const u64 at = StagingBytes - NoiseBytes;
        std::memcpy(g_staging.mapped + at, reinterpret_cast<const void*>(frame.noise_source),
                    NoiseBytes);
        cmd.copyBuffer(g_staging.buffer, g_noise.buffer,
                       vk::BufferCopy{.srcOffset = at, .size = NoiseBytes}, dl);
        g_noise_copied = true;
    }
    CopyLibrary(cmd, frame);
    CopyLiveDescriptions(cmd, frame, packet, buffer, offset);
    CopyCurves(cmd);
    FullBarrier(cmd);

    const u32 last = g_next;
    const u32 next = 1 - last;
    const u64 effects = address + Scene::ParticleEffectsOffset;
    const u64 owners = address + Scene::ParticleOwnersOffset;
    const u32 object_groups = Groups(frame.objects);
    const u32 particle_groups = Groups(frame.particles);

    const UpdatePush update{
        .emitter_in = g_objects[last].address,
        .effect_in = effects,
        .owner_in = owners,
        .description = g_description.address,
        .emitter_out = g_objects[next].address,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_update.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_update.layout, 0, g_compute_curve_set,
                           {}, dl);
    cmd.pushConstants(g_update.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(update),
                      &update, dl);
    cmd.dispatch(object_groups, 1, 1, dl);
    FullBarrier(cmd);

    const u64 clear_out = g_particles[next].address;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_clear.pipeline, dl);
    cmd.pushConstants(g_clear.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(clear_out),
                      &clear_out, dl);
    cmd.dispatch(particle_groups, 1, 1, dl);
    FullBarrier(cmd);

    const MovePush move{
        .particle_in = g_particles[last].address,
        .emitter_in = g_objects[next].address,
        .effect_in = effects,
        .description = g_description.address,
        .particle_out = g_particles[next].address,
        .per_scene = address + Scene::ParticleSceneOffset,
        .noise = g_noise.address,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_move.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_move.layout, 0, g_compute_depth_set,
                           {}, dl);
    cmd.pushConstants(g_move.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(move), &move, dl);
    if (g_frames >= g_depth_ready_frame) {
        cmd.dispatch(particle_groups, 1, 1, dl);
    }
    FullBarrier(cmd);

    const SpawnPush spawn{
        .effect_in = effects,
        .description = g_description.address,
        .particle_out = g_particles[next].address,
        .emitter_out = g_objects[next].address,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_spawn.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_spawn.layout, 0, g_compute_curve_set,
                           {}, dl);
    cmd.pushConstants(g_spawn.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(spawn), &spawn,
                      dl);
    cmd.dispatch(object_groups, 1, 1, dl);
    FullBarrier(cmd);
    DeviceLost::Mark(*g_device, cmd, DeviceLost::Pass::MeshSampling);
    RecordSamples(cmd, frame, packet, address, next);
    g_next = next;
    CheckAlive(cmd, frame, g_particles[next]);
}

void RecordCollision(vk::CommandBuffer cmd, const Image& depth) {
    if (!g_ready.load(std::memory_order_acquire) || !g_collision.image) {
        return;
    }
    const auto& dl = g_device->dispatch;
    if (!g_collision_cleared) {
        ClearCollision(cmd);
    }
    constexpr auto Tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
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
            .subresourceRange = DepthRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dl);
    };
    depth_barrier(vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  vk::ImageLayout::eDepthStencilReadOnlyOptimal, Tests,
                  vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
                  vk::PipelineStageFlagBits2::eComputeShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
    // The last frame's move program read the copy.
    FullBarrier(cmd);
    // The game's program runs groups of 10 by 10 pixels.
    constexpr u32 GroupPixels = 10;
    const u64 size = g_collision_size.address;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_collision_backup.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_collision_backup.layout, 0,
                           g_collision_set, {}, dl);
    cmd.pushConstants(g_collision_backup.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                      &size, dl);
    cmd.dispatch((CollisionExtent.width + GroupPixels - 1) / GroupPixels,
                 (CollisionExtent.height + GroupPixels - 1) / GroupPixels, 1, dl);
    depth_barrier(vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                  vk::ImageLayout::eDepthStencilAttachmentOptimal,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eNone, Tests,
                  vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    FullBarrier(cmd);
}

void RecordEmitterColors(vk::CommandBuffer cmd, const Scene::Header& header, u64 address) {
    const Scene::ParticleHeader& frame = header.particles;
    if (!g_ready.load(std::memory_order_acquire) || !g_pool_ready || frame.objects == 0 ||
        !g_color_copy.image) {
        return;
    }
    const auto& dl = g_device->dispatch;
    if (!g_color_copy_cleared) {
        // Until the first copy, emitters stay white.
        g_color_copy_cleared = true;
        const vk::ImageMemoryBarrier2 layout{
            .dstStageMask = vk::PipelineStageFlagBits2::eClear,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = g_color_copy.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &layout}, dl);
        cmd.clearColorImage(g_color_copy.image, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{.float32 = std::array<float, 4>{1, 1, 1, 1}},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                            dl);
    }
    FullBarrier(cmd);
    const EmitterColorPush colors{
        .emitter_in = g_objects[g_next].address,
        .description = g_description.address,
        .color_out = g_colors.address,
        .per_scene = address + Scene::ParticleColorSceneOffset,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_emitter_color.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_emitter_color.layout, 0,
                           g_compute_color_set, {}, dl);
    cmd.pushConstants(g_emitter_color.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(colors),
                      &colors, dl);
    cmd.dispatch(Groups(frame.objects), 1, 1, dl);
    FullBarrier(cmd);

    constexpr u32 CopyGroupPixels = 8;
    const u64 size = g_color_copy_size.address;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_color_backup.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_color_backup.layout, 0,
                           g_color_backup_set, {}, dl);
    cmd.pushConstants(g_color_backup.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(size),
                      &size, dl);
    cmd.dispatch((g_color_copy.extent.width + CopyGroupPixels - 1) / CopyGroupPixels,
                 (g_color_copy.extent.height + CopyGroupPixels - 1) / CopyGroupPixels, 1, dl);
    FullBarrier(cmd);
}

void RecordSelfTranslucent(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                           u64 address, vk::Buffer buffer, u64 offset) {
    const Scene::ParticleHeader& frame = header.particles;
    if (!g_ready.load(std::memory_order_acquire) || !g_pool_ready || frame.runs == 0 ||
        g_frames < g_depth_ready_frame) {
        return;
    }
    const auto& dl = g_device->dispatch;
    const DrawPush push = MakePush(address);
    std::array<Scene::ParticleRun, Scene::MaxParticleDraws> runs;
    std::memcpy(runs.data(), packet + Scene::ParticleRunsOffset,
                frame.runs * sizeof(Scene::ParticleRun));
    u32 bound = ~0u;
    for (u32 pass = 0; pass < Scene::SelfTranslucentLists; pass++) {
        cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, pass + 1, dl);
        for (u32 i = 0; i < frame.runs; i++) {
            const Scene::ParticleRun& run = runs[i];
            if (run.mode != SelfTranslucentFirstMode + pass ||
                run.pipeline >= SelfTranslucentPrograms) {
                continue;
            }
            const u32 inside = run.pipeline * 2 + run.tested;
            if (inside != bound) {
                bound = inside;
                const Pipeline& pipeline = g_self_translucent[inside];
                const std::array sets{Textures::Set(), g_draw_set};
                cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets,
                                       {}, dl);
                cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                                  sizeof(push), &push, dl);
            }
            cmd.drawIndirect(buffer,
                             offset + Scene::ParticleCommandsOffset +
                                 run.first * sizeof(Scene::VertexCommand),
                             run.count, sizeof(Scene::VertexCommand), dl);
        }
    }
}

void RecordShadows(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                   u64 address, vk::Buffer buffer, u64 offset, u32 cascade) {
    const Scene::ParticleHeader& frame = header.particles;
    if (!g_ready.load(std::memory_order_acquire) || !g_pool_ready || !g_depth_copy_ready ||
        frame.shadow_runs == 0) {
        return;
    }
    DrawOwnRuns(cmd, frame, packet, address, buffer, offset, Scene::ParticleShadowMode + cascade,
                g_shadow_pipelines);
}

void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                    u64 address, vk::Buffer buffer, u64 offset) {
    const Scene::ParticleHeader& frame = header.particles;
    if (!g_ready.load(std::memory_order_acquire) || !g_pool_ready || !g_depth_copy_ready ||
        frame.surface_runs == 0) {
        return;
    }
    DrawOwnRuns(cmd, frame, packet, address, buffer, offset, Scene::ParticleSurfaceMode,
                g_surface_pipelines);
}

bool HasDraws(const Scene::Header& header, const u8* packet, Group group) {
    const Scene::ParticleHeader& frame = header.particles;
    std::array<Scene::ParticleRun, Scene::MaxParticleDraws> runs;
    std::memcpy(runs.data(), packet + Scene::ParticleRunsOffset,
                frame.runs * sizeof(Scene::ParticleRun));
    const Modes modes = ModesOf(group);
    return std::any_of(runs.begin(), runs.begin() + frame.runs,
                       [&](const Scene::ParticleRun& run) { return Holds(modes, run.mode); });
}

void RecordDraws(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
                 vk::Buffer buffer, u64 offset, vk::ImageView scene, const Image& depth,
                 vk::Extent2D extent, Group group) {
    const Scene::ParticleHeader& frame = header.particles;
    if (!g_ready.load(std::memory_order_acquire) || !g_pool_ready || !g_depth_copy_ready ||
        frame.runs == frame.shadow_runs + frame.surface_runs) {
        return;
    }
    const auto& dl = g_device->dispatch;
    bool begun = false;
    const DrawPush push = MakePush(address);
    std::array<Scene::ParticleRun, Scene::MaxParticleDraws> runs;
    std::memcpy(runs.data(), packet + Scene::ParticleRunsOffset,
                frame.runs * sizeof(Scene::ParticleRun));
    const Modes modes = ModesOf(group);
    const auto is_forward = [&modes](u32 mode) { return Holds(modes, mode); };
    // A draw that writes the depth needs the programs to read a copy of it.
    const bool writes_depth =
        std::any_of(runs.begin(), runs.begin() + frame.runs, [&](const Scene::ParticleRun& run) {
            return run.write != 0 && run.tested != 0 && is_forward(run.mode);
        });
    if (writes_depth) {
        CopyDepth(cmd, depth, extent);
    }
    const vk::DescriptorSet set = writes_depth ? g_copy_set : g_draw_set;
    u32 bound = ~0u;
    const auto draw_run = [&](const Scene::ParticleRun& run) {
        const u32 program = run.pipeline % ProgramCount;
        if (program >= FogFirstProgram && program < FogEndProgram && !g_fog_ready) {
            return;
        }
        if (!begun) {
            begun = true;
            const vk::MemoryBarrier2 colors{
                .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                                 vk::AccessFlagBits2::eColorAttachmentWrite,
            };
            cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &colors}, dl);
            const vk::RenderingAttachmentInfo color{
                .imageView = scene,
                .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .loadOp = vk::AttachmentLoadOp::eLoad,
                .storeOp = vk::AttachmentStoreOp::eStore,
            };
            const vk::RenderingAttachmentInfo depth_attachment{
                .imageView = depth.view,
                .imageLayout = writes_depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                            : vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                .loadOp = vk::AttachmentLoadOp::eLoad,
                .storeOp =
                    writes_depth ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eNone,
            };
            cmd.beginRendering({.renderArea = {{0, 0}, extent},
                                .layerCount = 1,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &color,
                                .pDepthAttachment = &depth_attachment},
                               dl);
            // A negative height keeps the game's y-up clip space.
            const vk::Viewport viewport{
                .x = 0.0f,
                .y = static_cast<float>(extent.height),
                .width = static_cast<float>(extent.width),
                .height = -static_cast<float>(extent.height),
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            };
            cmd.setViewport(0, viewport, dl);
            cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
            cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
        }
        const bool effect = run.pipeline == Scene::ParticleEffectPipeline;
        const bool opaque = run.mode == OpaqueMode && (effect || program < OpaquePrograms);
        // Pass 1's pipelines take keys after all the others.
        const u32 key = run.pipeline + (opaque ? Scene::ParticleEffectPipeline + 1 : 0);
        if (key != bound) {
            bound = key;
            const Pipeline& pipeline =
                effect   ? (opaque ? g_effect_opaque : g_effect)
                : opaque ? g_opaque[run.pipeline / ProgramCount * OpaquePrograms + program]
                         : g_draws[run.pipeline];
            const std::array sets{Textures::Set(), set};
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {},
                                   dl);
            cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                              sizeof(push), &push, dl);
        }
        SetRunState(cmd, run, writes_depth);
        DrawRun(cmd, run, buffer, offset);
    };
    const auto draw_modes = [&](std::span<const u32> modes) {
        for (const u32 mode : modes) {
            for (u32 i = 0; i < frame.runs; i++) {
                if (runs[i].mode == mode) {
                    draw_run(runs[i]);
                }
            }
        }
    };
    draw_modes(modes.before_copy);
    const bool reads_copy =
        std::any_of(runs.begin(), runs.begin() + frame.runs, [&modes](const auto& run) {
            return std::find(modes.of_copy.begin(), modes.of_copy.end(), run.mode) !=
                   modes.of_copy.end();
        });
    if (reads_copy) {
        if (begun) {
            cmd.endRendering(dl);
            begun = false;
        }
        CopyScene(cmd, extent);
    }
    draw_modes(modes.of_copy);
    draw_modes(modes.after_copy);
    if (begun) {
        cmd.endRendering(dl);
    }
    if (writes_depth) {
        constexpr auto Tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = Tests,
            .srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .dstStageMask = Tests | vk::PipelineStageFlagBits2::eFragmentShader,
            .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                             vk::AccessFlagBits2::eShaderSampledRead,
            .oldLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
            .image = depth.image,
            .subresourceRange = DepthRange,
        };
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read}, dl);
    }
}

} // namespace Gr2Port::Particles
