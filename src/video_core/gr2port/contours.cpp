// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/gr2port/contours.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/contour_frag.h"
#include "video_core/gr2port/shaders/contour_geom.h"
#include "video_core/gr2port/shaders/contour_vert.h"

namespace Gr2Port::Contours {

namespace {

const Device* g_device = nullptr;
Pipeline g_pipeline;

} // namespace

bool Start(const Device& device, vk::Format color_format, vk::Format depth_format) {
    g_device = &device;
    if (!device.geometry_shaders) {
        return true;
    }
    g_pipeline = CreateGraphicsPipeline(
        device, {
                    .vertex = CONTOUR_VERT,
                    .geometry = CONTOUR_GEOM,
                    .fragment = CONTOUR_FRAG,
                    .color = color_format,
                    .depth = depth_format,
                    .topology = vk::PrimitiveTopology::eTriangleListWithAdjacency,
                    .depth_test = true,
                    .depth_write = false,
                    .depth_compare = vk::CompareOp::eGreaterOrEqual,
                    .alpha_blend = true,
                    .clear_target_alpha = true,
                    .push_constant_bytes = sizeof(u64),
                });
    return static_cast<bool>(g_pipeline.pipeline);
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, vk::Extent2D extent) {
    if (!g_pipeline.pipeline || header.contours == 0) {
        return;
    }
    const auto& dl = g_device->dispatch;
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport{
        .x = 0.0f,
        .y = static_cast<float>(extent.height),
        .width = static_cast<float>(extent.width),
        .height = -static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const u64 contours = address + Scene::ContoursOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_pipeline.pipeline, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(contours),
                      &contours, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    cmd.drawIndexedIndirect(buffer, offset + Scene::ContourCommandsOffset, header.contours,
                            sizeof(Scene::Command), dl);
}

} // namespace Gr2Port::Contours
