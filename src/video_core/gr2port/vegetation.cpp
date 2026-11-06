// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "video_core/gr2port/deformers.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/branch_comp.h"
#include "video_core/gr2port/shaders/branch_frag.h"
#include "video_core/gr2port/shaders/branch_vert.h"
#include "video_core/gr2port/shaders/grass_comp.h"
#include "video_core/gr2port/shaders/grass_frag.h"
#include "video_core/gr2port/shaders/grass_vert.h"
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

struct KernelPush {
    u64 grass;
    u64 blades;
    u64 effectors;
    u64 grid;
    u64 vertex_count;
};

struct GrassPush {
    u64 grass;
    u64 blades;
};

struct BranchKernelPush {
    u64 jobs;
    u64 effectors;
    u64 grid;
};

const Device* g_device = nullptr;
Buffer g_blades;
Pipeline g_kernel;
Pipeline g_grass;
Pipeline g_branch_kernel;
Pipeline g_branches;
Pipeline g_leaves;
Pipeline g_leaf_shadows;

void Bind(vk::CommandBuffer cmd, const Pipeline& pipeline, const void* push, u32 push_bytes) {
    const auto& dl = g_device->dispatch;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, Textures::Set(),
                           {}, dl);
    cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, push_bytes, push,
                      dl);
}

} // namespace

bool Start(const Device& device, Memory& memory, std::span<const vk::Format> surface_formats,
           vk::Format depth_format, vk::Format shadow_format) {
    g_device = &device;
    const std::array texture_set{Textures::SetLayout()};
    g_blades = memory.CreateBuffer(u64{Scene::MaxBlades} * Scene::BladeBytes,
                                   vk::BufferUsageFlagBits::eStorageBuffer, Placement::Device);
    g_kernel = CreateComputePipeline(device, GRASS_COMP, sizeof(KernelPush));
    g_grass = CreateGraphicsPipeline(device, {
                                                 .vertex = GRASS_VERT,
                                                 .fragment = GRASS_FRAG,
                                                 .colors = surface_formats,
                                                 .depth = depth_format,
                                                 .depth_test = true,
                                                 .push_constant_bytes = sizeof(GrassPush),
                                                 .set_layouts = texture_set,
                                             });
    g_branch_kernel = CreateComputePipeline(device, BRANCH_COMP, sizeof(BranchKernelPush));
    g_branches = CreateGraphicsPipeline(device, {
                                                    .vertex = BRANCH_VERT,
                                                    .fragment = BRANCH_FRAG,
                                                    .colors = surface_formats,
                                                    .depth = depth_format,
                                                    .depth_test = true,
                                                    .push_constant_bytes = sizeof(u64),
                                                    .set_layouts = texture_set,
                                                });
    if (!g_blades.buffer || !g_kernel.pipeline || !g_grass.pipeline || !g_branch_kernel.pipeline ||
        !g_branches.pipeline) {
        return false;
    }
    if (!device.tessellation_shaders || !device.geometry_shaders) {
        return true;
    }
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

void RecordKernels(vk::CommandBuffer cmd, const Scene::Header& header, u64 address) {
    if (header.grass == 0 && header.branch_jobs == 0) {
        return;
    }
    const auto& dl = g_device->dispatch;
    // The grid was just computed, and earlier frames wrote and drew the blades and the branches.
    const vk::MemoryBarrier2 before{
        .srcStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eVertexShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before}, dl);
    if (header.branch_jobs != 0) {
        const BranchKernelPush push{
            .jobs = address + Scene::BranchJobsOffset,
            .effectors = address + Scene::EffectorsOffset,
            .grid = Deformers::GridAddress(),
        };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_branch_kernel.pipeline, dl);
        cmd.pushConstants(g_branch_kernel.layout, vk::ShaderStageFlagBits::eCompute, 0,
                          sizeof(push), &push, dl);
        cmd.dispatch(header.branch_groups, header.branch_jobs, 1, dl);
    }
    if (header.grass != 0) {
        const KernelPush push{
            .grass = address + Scene::GrassOffset,
            .blades = g_blades.address,
            .effectors = address + Scene::EffectorsOffset,
            .grid = Deformers::GridAddress(),
            .vertex_count = address + Scene::GrassCommandOffset,
        };
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_kernel.pipeline, dl);
        cmd.pushConstants(g_kernel.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                          &push, dl);
        cmd.dispatch(header.grass_groups, header.grass, 1, dl);
    }
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eDrawIndirect | vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eIndirectCommandRead | vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after}, dl);
}

void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                    vk::Buffer buffer, u64 offset) {
    if (header.grass != 0) {
        const GrassPush push{
            .grass = address + Scene::GrassOffset,
            .blades = g_blades.address,
        };
        Bind(cmd, g_grass, &push, sizeof(push));
        cmd.drawIndirect(buffer, offset + Scene::GrassCommandOffset, 1,
                         sizeof(Scene::VertexCommand), g_device->dispatch);
    }
    if (header.branches != 0) {
        const u64 branches = address + Scene::BranchesOffset;
        Bind(cmd, g_branches, &branches, sizeof(branches));
        cmd.drawIndirect(buffer, offset + Scene::BranchCommandsOffset, header.branches,
                         sizeof(Scene::VertexCommand), g_device->dispatch);
    }
    if (!g_leaves.pipeline || header.leaf_commands == 0) {
        return;
    }
    const u64 leaves = address + Scene::LeavesOffset;
    Bind(cmd, g_leaves, &leaves, sizeof(leaves));
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
    Bind(cmd, g_leaf_shadows, &push, sizeof(push));
    constexpr u64 Bytes = sizeof(Scene::VertexCommand);
    cmd.drawIndirect(
        buffer, offset + Scene::LeafShadowCommandsOffset + u64{cascade} * Scene::MaxLeaves * Bytes,
        count, Bytes, g_device->dispatch);
}

} // namespace Gr2Port::Vegetation
