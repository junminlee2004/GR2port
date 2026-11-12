// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>

#include "graphics/gr2port/device.h"

namespace Gr2Port {

struct GraphicsPipelineInfo {
    std::span<const u32> vertex;
    /// None for no tessellation stages; with them, the topology is a list of patches of
    /// patch_control_points.
    std::span<const u32> tessellation_control;
    std::span<const u32> tessellation_evaluation;
    u32 patch_control_points = 0;
    /// None for no geometry stage.
    std::span<const u32> geometry;
    /// None for a depth-only pipeline.
    std::span<const u32> fragment;
    /// Undefined for no colour attachment. colors, when not empty, replaces it with several.
    vk::Format color = vk::Format::eUndefined;
    std::span<const vk::Format> colors;
    vk::Format depth = vk::Format::eUndefined;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    vk::CullModeFlags cull = vk::CullModeFlagBits::eNone;
    bool depth_test = false;
    /// With depth_test, whether passing fragments write depth.
    bool depth_write = true;
    /// Greater for reverse depth, where nearer is greater.
    vk::CompareOp depth_compare = vk::CompareOp::eGreater;
    /// The depth bias is enabled and set as dynamic state.
    bool depth_bias = false;
    /// depth also holds stencil, whose reference is dynamic state: fragments that pass the depth
    /// test write the reference, or only fragments where the stencil equals it pass.
    bool stencil_replace = false;
    bool stencil_equal = false;
    /// depth also holds stencil, which the pipeline leaves alone.
    bool stencil_attachment = false;
    /// Colour blends over the target by the fragment's alpha.
    bool alpha_blend = false;
    /// The target keeps its alpha, with or without a blend.
    bool keep_target_alpha = false;
    /// Colour adds to the target scaled by one minus the fragment's alpha; the target keeps its
    /// alpha.
    bool premultiplied_blend = false;
    /// With a blend, every fragment sets the target's alpha to 0 instead.
    bool clear_target_alpha = false;
    /// Replaces the blend options above.
    const vk::PipelineColorBlendAttachmentState* blend = nullptr;
    /// The cull mode and the depth test and write are dynamic state.
    bool dynamic_depth_and_cull = false;
    /// The blend constants are dynamic state.
    bool dynamic_blend_constants = false;
    /// The primitive topology is dynamic state, of topology's class.
    bool dynamic_topology = false;
    /// The blend enable and equation are dynamic state; the device has dynamic_blend.
    bool dynamic_blend = false;
    u32 push_constant_bytes = 0;
    /// The pipeline's descriptor sets, in set order.
    std::span<const vk::DescriptorSetLayout> set_layouts;
};

struct Pipeline {
    vk::Pipeline pipeline;
    vk::PipelineLayout layout;
};

/// A pipeline for dynamic rendering with a dynamic viewport and scissor, no vertex input (the
/// shaders pull their data by device address) and push constants for every stage.
Pipeline CreateGraphicsPipeline(const Device& device, const GraphicsPipelineInfo& info);

/// A compute pipeline with push_constant_bytes of push constants and the given descriptor sets, its
/// specialization constants set by specialization when given.
Pipeline CreateComputePipeline(const Device& device, std::span<const u32> code,
                               u32 push_constant_bytes,
                               std::span<const vk::DescriptorSetLayout> set_layouts = {},
                               const vk::SpecializationInfo* specialization = nullptr);

void DestroyPipeline(const Device& device, Pipeline& pipeline);

/// The stages of the pipeline the calling thread is building, each with the FNV-1a sum of its
/// code and its length, for the report of a fault inside the driver; empty outside a build.
std::string PipelineBeingBuilt();

/// After the device is made: creates its pipeline cache from the file an earlier run saved.
void LoadPipelineCache(Device& device);

/// Any thread: writes the pipeline cache to its file, with every pipeline made so far, when the
/// cache holds pipelines the file does not.
void SavePipelineCache(const Device& device);

} // namespace Gr2Port
