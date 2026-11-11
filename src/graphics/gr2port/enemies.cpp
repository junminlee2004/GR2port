// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <thread>

#include "common/logging/log.h"
#include "graphics/gr2port/enemies.h"
#include "graphics/gr2port/meshes.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/point_lights.h"
#include "graphics/gr2port/self_translucent.h"
#include "graphics/gr2port/shaders/enemy_boss_frag.h"
#include "graphics/gr2port/shaders/enemy_boss_skinned_vert.h"
#include "graphics/gr2port/shaders/enemy_boss_vert.h"
#include "graphics/gr2port/shaders/enemy_core_alpha_frag.h"
#include "graphics/gr2port/shaders/enemy_core_frag.h"
#include "graphics/gr2port/shaders/enemy_core_skinned_vert.h"
#include "graphics/gr2port/shaders/enemy_core_vert.h"
#include "graphics/gr2port/shaders/enemy_frag.h"
#include "graphics/gr2port/shaders/enemy_skinned_vert.h"
#include "graphics/gr2port/shaders/enemy_vert.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/upscaler.h"

namespace Gr2Port::Enemies {

namespace {

struct DrawPush {
    u64 draws;
    u64 point_lights;
    u64 light_grid;
};

const Device* g_device = nullptr;
vk::Format g_scene_format;
vk::Format g_depth_format;
std::atomic<bool> g_ready{false};
std::thread g_builder;
// By list, with the game's programs, and for the lists the scene draws, with the vertex program
// alone. Lists 6 and 7 draw into SelfTranslucent's targets, 8 and 9 after the sky.
std::array<Pipeline, Scene::EnemyLists> g_forward;
std::array<Pipeline, Scene::EnemySelfTranslucentLists> g_depth;

// Builds the pipelines, whose programs run to thousands of instructions.
void Build() {
    const auto start = std::chrono::steady_clock::now();
    const std::array<std::span<const u32>, Scene::EnemyLists> vertex{
        ENEMY_VERT,      ENEMY_SKINNED_VERT,      ENEMY_BOSS_VERT, ENEMY_BOSS_SKINNED_VERT,
        ENEMY_CORE_VERT, ENEMY_CORE_SKINNED_VERT, ENEMY_CORE_VERT, ENEMY_CORE_SKINNED_VERT,
        ENEMY_CORE_VERT, ENEMY_CORE_SKINNED_VERT,
    };
    const std::array<std::span<const u32>, Scene::EnemyLists> fragment{
        ENEMY_FRAG,
        ENEMY_FRAG,
        ENEMY_BOSS_FRAG,
        ENEMY_BOSS_FRAG,
        ENEMY_CORE_FRAG,
        ENEMY_CORE_FRAG,
        ENEMY_CORE_FRAG,
        ENEMY_CORE_FRAG,
        ENEMY_CORE_ALPHA_FRAG,
        ENEMY_CORE_ALPHA_FRAG,
    };
    const std::array forward_sets{Textures::SetLayout(), SelfTranslucent::SetLayout()};
    const std::array depth_sets{Textures::SetLayout()};
    bool built = true;
    for (u32 list = 0; list < Scene::EnemySelfTranslucentLists; list++) {
        g_forward[list] =
            CreateGraphicsPipeline(*g_device, {
                                                  .vertex = vertex[list],
                                                  .fragment = fragment[list],
                                                  .color = g_scene_format,
                                                  .depth = g_depth_format,
                                                  .cull = vk::CullModeFlagBits::eBack,
                                                  .depth_test = true,
                                                  .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                                  .push_constant_bytes = sizeof(DrawPush),
                                                  .set_layouts = forward_sets,
                                              });
        g_depth[list] =
            CreateGraphicsPipeline(*g_device, {
                                                  .vertex = vertex[list],
                                                  .depth = g_depth_format,
                                                  .cull = vk::CullModeFlagBits::eBack,
                                                  .depth_test = true,
                                                  .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                                  .push_constant_bytes = sizeof(DrawPush),
                                                  .set_layouts = depth_sets,
                                              });
        built = built && g_forward[list].pipeline && g_depth[list].pipeline;
    }
    // In SelfTranslucent's targets the game draws the cores with its state for the effects inside
    // the skin: no culling, and the stencil set to its reference.
    const std::array self_translucent_sets{Textures::SetLayout()};
    for (u32 list = Scene::EnemySelfTranslucentLists; list < Scene::EnemyAlphaLists; list++) {
        g_forward[list] =
            CreateGraphicsPipeline(*g_device, {
                                                  .vertex = vertex[list],
                                                  .fragment = fragment[list],
                                                  .color = SelfTranslucent::ColorFormat,
                                                  .depth = SelfTranslucent::DepthFormat,
                                                  .depth_test = true,
                                                  .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                                  .stencil_replace = true,
                                                  .push_constant_bytes = sizeof(DrawPush),
                                                  .set_layouts = self_translucent_sets,
                                              });
        built = built && g_forward[list].pipeline;
    }
    // After the sky the game blends the cores again by their alpha, without depth writes, and
    // keeps the target's alpha.
    constexpr vk::PipelineColorBlendAttachmentState AlphaBlend{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vk::BlendFactor::eOne,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const std::array alpha_sets{Textures::SetLayout()};
    for (u32 list = Scene::EnemyAlphaLists; list < Scene::EnemyLists; list++) {
        g_forward[list] =
            CreateGraphicsPipeline(*g_device, {
                                                  .vertex = vertex[list],
                                                  .fragment = fragment[list],
                                                  .color = g_scene_format,
                                                  .depth = g_depth_format,
                                                  .cull = vk::CullModeFlagBits::eBack,
                                                  .depth_test = true,
                                                  .depth_write = false,
                                                  .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                                  .blend = &AlphaBlend,
                                                  .push_constant_bytes = sizeof(DrawPush),
                                                  .set_layouts = alpha_sets,
                                              });
        built = built && g_forward[list].pipeline;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!built) {
        LOG_ERROR(Render_Vulkan, "GR2port enemies: a pipeline failed, after {} ms", ms);
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2port enemies: pipelines built in {} ms", ms);
    SavePipelineCache(*g_device);
    g_ready.store(true, std::memory_order_release);
}

// Draws the commands of the lists from first_list to before end_list, each with its pipeline in
// pipelines, with the sets bound.
void Draw(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
          u64 offset, vk::Extent2D extent, std::span<const Pipeline> pipelines, u32 first_list,
          u32 end_list, std::span<const vk::DescriptorSet> sets) {
    u32 commands = 0;
    for (u32 list = first_list; list < end_list; list++) {
        commands += header.enemy_commands[list];
    }
    if (commands == 0 || !g_ready.load(std::memory_order_acquire)) {
        return;
    }
    const auto& dl = g_device->dispatch;
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport = Upscaler::SceneViewport(extent);
    const DrawPush push{
        .draws = address + Scene::EnemyDrawsOffset,
        .point_lights = address + Scene::PointLightsOffset,
        .light_grid = PointLights::GridAddress(),
    };
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    for (u32 list = first_list; list < end_list; list++) {
        const u32 count = header.enemy_commands[list];
        if (count == 0) {
            continue;
        }
        const Pipeline& pipeline = pipelines[list];
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {}, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        cmd.drawIndexedIndirect(buffer,
                                offset + Scene::EnemyCommandsOffset +
                                    u64{list} * Scene::MaxEnemyDraws * sizeof(Scene::Command),
                                count, sizeof(Scene::Command), dl);
    }
}

} // namespace

bool Start(const Device& device, vk::Format scene_format, vk::Format depth_format) {
    g_device = &device;
    g_scene_format = scene_format;
    g_depth_format = depth_format;
    g_builder = std::thread{Build};
    g_builder.detach();
    return true;
}

void RecordDepth(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
                 u64 offset, vk::Extent2D extent) {
    const std::array sets{Textures::Set()};
    Draw(cmd, header, address, buffer, offset, extent, g_depth, 0, Scene::EnemySelfTranslucentLists,
         sets);
}

void RecordForward(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                   vk::Buffer buffer, u64 offset, vk::Extent2D extent) {
    const std::array sets{Textures::Set(), SelfTranslucent::Set()};
    Draw(cmd, header, address, buffer, offset, extent, g_forward, 0,
         Scene::EnemySelfTranslucentLists, sets);
}

bool HasSelfTranslucent(const Scene::Header& header) {
    return header.enemy_commands[Scene::EnemySelfTranslucentLists] +
               header.enemy_commands[Scene::EnemySelfTranslucentLists + 1] !=
           0;
}

void RecordSelfTranslucent(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                           vk::Buffer buffer, u64 offset, vk::Extent2D extent) {
    // The game's stencil reference for the cores, after the effects' 1 and 2.
    constexpr u32 CoreStencil = 3;
    cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, CoreStencil,
                            g_device->dispatch);
    const std::array sets{Textures::Set()};
    Draw(cmd, header, address, buffer, offset, extent, g_forward, Scene::EnemySelfTranslucentLists,
         Scene::EnemyAlphaLists, sets);
}

void RecordAlpha(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
                 u64 offset, vk::Extent2D extent) {
    const std::array sets{Textures::Set()};
    Draw(cmd, header, address, buffer, offset, extent, g_forward, Scene::EnemyAlphaLists,
         Scene::EnemyLists, sets);
}

} // namespace Gr2Port::Enemies
