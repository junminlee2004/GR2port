// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "video_core/gr2_pt/device.h"

namespace Gr2Pt {

struct GraphicsPipelineInfo {
    std::span<const u32> vertex;
    std::span<const u32> fragment;
    vk::Format color = vk::Format::eUndefined;
    vk::Format depth = vk::Format::eUndefined;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    vk::CullModeFlags cull = vk::CullModeFlagBits::eNone;
    /// Reverse depth: nearer is greater.
    bool depth_test = false;
    u32 push_constant_bytes = 0;
};

struct Pipeline {
    vk::Pipeline pipeline;
    vk::PipelineLayout layout;
};

/// A pipeline for dynamic rendering with a dynamic viewport and scissor, no vertex input (the
/// shaders pull their data by device address) and push constants for every stage.
Pipeline CreateGraphicsPipeline(const Device& device, const GraphicsPipelineInfo& info);

void DestroyPipeline(const Device& device, Pipeline& pipeline);

} // namespace Gr2Pt
