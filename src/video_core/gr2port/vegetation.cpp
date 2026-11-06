// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/leaf_frag.h"
#include "video_core/gr2port/shaders/leaf_geom.h"
#include "video_core/gr2port/shaders/leaf_shadow_geom.h"
#include "video_core/gr2port/shaders/leaf_shadow_tese.h"
#include "video_core/gr2port/shaders/leaf_tesc.h"
#include "video_core/gr2port/shaders/leaf_tese.h"
#include "video_core/gr2port/shaders/leaf_vert.h"
#include "video_core/gr2port/shaders/shadow_alpha_frag.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/vegetation.h"

namespace Gr2Port::Vegetation {

namespace {

// A branch's patches are quadratic Bezier curves of three control points.
constexpr u32 PatchControlPoints = 3;

struct ShadowPush {
    u64 leaves;
    u64 shadows;
    u32 cascade;
    u32 padding;
};

const Device* g_device = nullptr;
Pipeline g_leaves;
Pipeline g_leaf_shadows;

void BindLeaves(vk::CommandBuffer cmd, const Pipeline& pipeline, const void* push, u32 push_bytes) {
    const auto& dl = g_device->dispatch;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, Textures::Set(),
                           {}, dl);
    cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, push_bytes, push,
                      dl);
}

} // namespace

bool Start(const Device& device, std::span<const vk::Format> surface_formats,
           vk::Format depth_format, vk::Format shadow_format) {
    g_device = &device;
    if (!device.tessellation_shaders || !device.geometry_shaders) {
        return true;
    }
    const std::array texture_set{Textures::SetLayout()};
    g_leaves = CreateGraphicsPipeline(device, {
                                                  .vertex = LEAF_VERT,
                                                  .tessellation_control = LEAF_TESC,
                                                  .tessellation_evaluation = LEAF_TESE,
                                                  .patch_control_points = PatchControlPoints,
                                                  .geometry = LEAF_GEOM,
                                                  .fragment = LEAF_FRAG,
                                                  .colors = surface_formats,
                                                  .depth = depth_format,
                                                  .depth_test = true,
                                                  .push_constant_bytes = sizeof(u64),
                                                  .set_layouts = texture_set,
                                              });
    g_leaf_shadows =
        CreateGraphicsPipeline(device, {
                                           .vertex = LEAF_VERT,
                                           .tessellation_control = LEAF_TESC,
                                           .tessellation_evaluation = LEAF_SHADOW_TESE,
                                           .patch_control_points = PatchControlPoints,
                                           .geometry = LEAF_SHADOW_GEOM,
                                           .fragment = SHADOW_ALPHA_FRAG,
                                           .depth = shadow_format,
                                           .depth_test = true,
                                           .depth_compare = vk::CompareOp::eLessOrEqual,
                                           .depth_bias = true,
                                           .push_constant_bytes = sizeof(ShadowPush),
                                           .set_layouts = texture_set,
                                       });
    return g_leaves.pipeline && g_leaf_shadows.pipeline;
}

void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                    vk::Buffer buffer, u64 offset) {
    if (!g_leaves.pipeline || header.leaf_commands == 0) {
        return;
    }
    const u64 leaves = address + Scene::LeavesOffset;
    BindLeaves(cmd, g_leaves, &leaves, sizeof(leaves));
    cmd.drawIndirect(buffer, offset + Scene::LeafCommandsOffset, header.leaf_commands,
                     sizeof(Scene::VertexCommand), g_device->dispatch);
}

void RecordShadows(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                   vk::Buffer buffer, u64 offset, u32 cascade) {
    const u32 count = header.leaf_shadow_commands[cascade];
    if (!g_leaf_shadows.pipeline || count == 0) {
        return;
    }
    const ShadowPush push{
        .leaves = address + Scene::LeavesOffset,
        .shadows = address + Scene::ShadowsOffset,
        .cascade = cascade,
    };
    BindLeaves(cmd, g_leaf_shadows, &push, sizeof(push));
    constexpr u64 Bytes = sizeof(Scene::VertexCommand);
    cmd.drawIndirect(
        buffer, offset + Scene::LeafShadowCommandsOffset + u64{cascade} * Scene::MaxLeaves * Bytes,
        count, Bytes, g_device->dispatch);
}

} // namespace Gr2Port::Vegetation
