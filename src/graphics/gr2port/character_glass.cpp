// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>

#include "common/logging/log.h"
#include "graphics/gr2port/character_glass.h"
#include "graphics/gr2port/effects.h"
#include "graphics/gr2port/meshes.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/shaders/character_glass_frag.h"
#include "graphics/gr2port/shaders/character_glass_skinned_vert.h"
#include "graphics/gr2port/shaders/character_glass_vert.h"
#include "graphics/gr2port/shadows.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/upscaler.h"

namespace Gr2Port::CharacterGlass {

namespace {

struct DrawPush {
    u64 draws;
};

constexpr u32 BlendModes = 8;

const Device* g_device = nullptr;
vk::Format g_scene_format;
vk::Format g_depth_format;
std::atomic<bool> g_ready{false};
// By whether the mesh is skinned, then by blend mode.
std::array<std::array<Pipeline, BlendModes>, 2> g_pipelines;

// Builds the pipelines, whose programs run to thousands of instructions.
void Build() {
    const auto start = std::chrono::steady_clock::now();
    const std::array<std::span<const u32>, 2> vertex{CHARACTER_GLASS_VERT,
                                                     CHARACTER_GLASS_SKINNED_VERT};
    const std::array sets{Textures::SetLayout(), Shadows::SetLayout()};
    bool built = true;
    for (u32 skinned = 0; skinned < vertex.size(); skinned++) {
        for (u32 mode = 0; mode < BlendModes; mode++) {
            // The scene's alpha marks the antialias history's pixels, as the game's stencil does,
            // which the glass leaves alone.
            vk::PipelineColorBlendAttachmentState blend = Effects::BlendState(mode);
            blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
            blend.dstAlphaBlendFactor = vk::BlendFactor::eOne;
            blend.alphaBlendOp = vk::BlendOp::eAdd;
            g_pipelines[skinned][mode] = CreateGraphicsPipeline(
                *g_device, {
                               .vertex = vertex[skinned],
                               .fragment = CHARACTER_GLASS_FRAG,
                               .color = g_scene_format,
                               .depth = g_depth_format,
                               .cull = vk::CullModeFlagBits::eBack,
                               .depth_test = true,
                               .depth_write = false,
                               .depth_compare = vk::CompareOp::eGreaterOrEqual,
                               .blend = &blend,
                               .push_constant_bytes = sizeof(DrawPush),
                               .set_layouts = sets,
                           });
            built = built && g_pipelines[skinned][mode].pipeline;
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!built) {
        LOG_ERROR(Render_Vulkan, "GR2port character glass: a pipeline failed, after {} ms", ms);
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2port character glass: pipelines built in {} ms", ms);
    SavePipelineCache(*g_device);
    g_ready.store(true, std::memory_order_release);
}

} // namespace

bool Start(const Device& device, vk::Format scene_format, vk::Format depth_format) {
    g_device = &device;
    g_scene_format = scene_format;
    g_depth_format = depth_format;
    StartPipelineBuild(Build);
    return true;
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* data, u64 address,
            vk::Buffer buffer, u64 offset, vk::Extent2D extent) {
    const u32 count = header.character_glass;
    if (count == 0 || !g_ready.load(std::memory_order_acquire)) {
        return;
    }
    const auto& dl = g_device->dispatch;
    std::array<u32, Scene::MaxCharacterGlassDraws> states;
    std::memcpy(states.data(), data + Scene::CharacterGlassStatesOffset, count * sizeof(u32));
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport = Upscaler::SceneViewport(extent);
    const DrawPush push{.draws = address + Scene::CharacterGlassDrawsOffset};
    const std::array sets{Textures::Set(), Shadows::Set()};
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    const Pipeline* bound = nullptr;
    for (u32 i = 0; i < count; i++) {
        const u32 skinned = (states[i] & Scene::CharacterGlassSkinned) != 0 ? 1 : 0;
        const Pipeline& pipeline = g_pipelines[skinned][states[i] % BlendModes];
        if (&pipeline != bound) {
            bound = &pipeline;
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {},
                                   dl);
            cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                              sizeof(push), &push, dl);
        }
        cmd.drawIndexedIndirect(
            buffer, offset + Scene::CharacterGlassCommandsOffset + u64{i} * sizeof(Scene::Command),
            1, sizeof(Scene::Command), dl);
    }
}

} // namespace Gr2Port::CharacterGlass
