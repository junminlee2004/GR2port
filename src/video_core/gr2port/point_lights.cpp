// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/point_lights.h"
#include "video_core/gr2port/shaders/light_grid_comp.h"

namespace Gr2Port::PointLights {

namespace {

constexpr u64 GridBytes = u64{Scene::LightGridColumns} * Scene::LightGridRows *
                          Scene::LightGridSlices * Scene::LightsPerCell * sizeof(u32);

struct GridPush {
    u64 light;
    u64 lights;
    u64 grid;
};

const Device* g_device = nullptr;
Pipeline g_pipeline;
Buffer g_grid;

} // namespace

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_pipeline = CreateComputePipeline(device, LIGHT_GRID_COMP, sizeof(GridPush));
    g_grid =
        memory.CreateBuffer(GridBytes, vk::BufferUsageFlagBits::eStorageBuffer, Placement::Device);
    return g_pipeline.pipeline && g_grid.buffer;
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address) {
    if (header.point_lights == 0) {
        return;
    }
    const auto& dl = g_device->dispatch;
    const GridPush push{
        .light = address + Scene::LightOffset,
        .lights = address + Scene::PointLightsOffset,
        .grid = g_grid.address,
    };
    // The last frame's passes read the grid.
    const vk::MemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &before}, dl);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_pipeline.pipeline, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push,
                      dl);
    cmd.dispatch(Scene::LightGridColumns, Scene::LightGridRows, Scene::LightGridSlices, dl);
    const vk::MemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &after}, dl);
}

u64 GridAddress() {
    return g_grid.address;
}

} // namespace Gr2Port::PointLights
