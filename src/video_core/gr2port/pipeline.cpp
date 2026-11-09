// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <filesystem>
#include <mutex>
#include <vector>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/gr2port/pipeline.h"

namespace Gr2Port {

namespace {

// The pipeline cache's file, in the emulator's shader folder, and the lock of its writes.
std::filesystem::path PipelineCachePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "gr2port_pipelines.bin";
}
std::mutex g_cache_file;

vk::ShaderModule CreateModule(const Device& d, std::span<const u32> code) {
    const vk::ShaderModuleCreateInfo info{
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    };
    return d.device.createShaderModule(info, nullptr, d.dispatch).value;
}

} // namespace

Pipeline CreateGraphicsPipeline(const Device& d, const GraphicsPipelineInfo& info) {
    const auto& dl = d.dispatch;
    Pipeline result;
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eAllGraphics,
        .size = info.push_constant_bytes,
    };
    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = static_cast<u32>(info.set_layouts.size()),
        .pSetLayouts = info.set_layouts.data(),
        .pushConstantRangeCount = info.push_constant_bytes != 0 ? 1u : 0u,
        .pPushConstantRanges = &push,
    };
    result.layout = d.device.createPipelineLayout(layout_info, nullptr, dl).value;

    const vk::ShaderModule vertex = CreateModule(d, info.vertex);
    const vk::ShaderModule fragment =
        info.fragment.empty() ? vk::ShaderModule{} : CreateModule(d, info.fragment);
    const vk::ShaderModule geometry =
        info.geometry.empty() ? vk::ShaderModule{} : CreateModule(d, info.geometry);
    const bool tessellated =
        !info.tessellation_control.empty() && !info.tessellation_evaluation.empty();
    const vk::ShaderModule control =
        tessellated ? CreateModule(d, info.tessellation_control) : vk::ShaderModule{};
    const vk::ShaderModule evaluation =
        tessellated ? CreateModule(d, info.tessellation_evaluation) : vk::ShaderModule{};
    std::vector<vk::PipelineShaderStageCreateInfo> stages{{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = vertex,
        .pName = "main",
    }};
    if (tessellated) {
        stages.push_back({
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = control,
            .pName = "main",
        });
        stages.push_back({
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = evaluation,
            .pName = "main",
        });
    }
    if (geometry) {
        stages.push_back({
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = geometry,
            .pName = "main",
        });
    }
    if (fragment) {
        stages.push_back({
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment,
            .pName = "main",
        });
    }
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = tessellated ? vk::PrimitiveTopology::ePatchList : info.topology};
    const vk::PipelineTessellationStateCreateInfo tessellation{.patchControlPoints =
                                                                   info.patch_control_points};
    const vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo rasterization{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = info.cull,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = info.depth_bias,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const bool stencil = info.stencil_replace || info.stencil_equal;
    const vk::StencilOpState stencil_state{
        .failOp = vk::StencilOp::eKeep,
        .passOp = info.stencil_replace ? vk::StencilOp::eReplace : vk::StencilOp::eKeep,
        .depthFailOp = vk::StencilOp::eKeep,
        .compareOp = info.stencil_replace ? vk::CompareOp::eAlways : vk::CompareOp::eEqual,
        .compareMask = 0xff,
        .writeMask = info.stencil_replace ? 0xffu : 0u,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil{
        .depthTestEnable = info.depth_test,
        .depthWriteEnable = info.depth_test && info.depth_write,
        .depthCompareOp = info.depth_compare,
        .stencilTestEnable = stencil,
        .front = stencil_state,
        .back = stencil_state,
    };
    const bool premultiplied = info.premultiplied_blend;
    const bool blended = info.alpha_blend || premultiplied;
    const bool keep_alpha = !info.clear_target_alpha && (premultiplied || info.keep_target_alpha);
    const vk::ColorComponentFlags color_only = vk::ColorComponentFlagBits::eR |
                                               vk::ColorComponentFlagBits::eG |
                                               vk::ColorComponentFlagBits::eB;
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = blended,
        .srcColorBlendFactor = premultiplied ? vk::BlendFactor::eOne : vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor =
            keep_alpha || info.clear_target_alpha ? vk::BlendFactor::eZero : vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = keep_alpha ? vk::BlendFactor::eOne : vk::BlendFactor::eZero,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask =
            keep_alpha && !blended ? color_only : color_only | vk::ColorComponentFlagBits::eA,
    };
    const std::span<const vk::Format> colors = !info.colors.empty() ? info.colors
                                               : info.color != vk::Format::eUndefined
                                                   ? std::span<const vk::Format>{&info.color, 1}
                                                   : std::span<const vk::Format>{};
    const std::vector<vk::PipelineColorBlendAttachmentState> blend_attachments(
        colors.size(), info.blend != nullptr ? *info.blend : blend_attachment);
    const u32 color_count = static_cast<u32>(colors.size());
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = color_count,
        .pAttachments = blend_attachments.data(),
    };
    std::vector<vk::DynamicState> dynamic_states{vk::DynamicState::eViewport,
                                                 vk::DynamicState::eScissor};
    if (info.depth_bias) {
        dynamic_states.push_back(vk::DynamicState::eDepthBias);
    }
    if (stencil) {
        dynamic_states.push_back(vk::DynamicState::eStencilReference);
    }
    if (info.dynamic_depth_and_cull) {
        dynamic_states.push_back(vk::DynamicState::eCullMode);
        dynamic_states.push_back(vk::DynamicState::eDepthTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthWriteEnable);
    }
    if (info.dynamic_blend_constants) {
        dynamic_states.push_back(vk::DynamicState::eBlendConstants);
    }
    if (info.dynamic_topology) {
        dynamic_states.push_back(vk::DynamicState::ePrimitiveTopology);
    }
    if (info.dynamic_blend) {
        dynamic_states.push_back(vk::DynamicState::eColorBlendEnableEXT);
        dynamic_states.push_back(vk::DynamicState::eColorBlendEquationEXT);
    }
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = color_count,
        .pColorAttachmentFormats = colors.data(),
        .depthAttachmentFormat = info.depth,
        .stencilAttachmentFormat =
            stencil || info.stencil_attachment ? info.depth : vk::Format::eUndefined,
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = tessellated ? &tessellation : nullptr,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = result.layout,
    };
    const auto [pipeline_result, pipeline] =
        d.device.createGraphicsPipeline(d.pipeline_cache, pipeline_info, nullptr, dl);
    d.device.destroyShaderModule(vertex, nullptr, dl);
    if (fragment) {
        d.device.destroyShaderModule(fragment, nullptr, dl);
    }
    if (geometry) {
        d.device.destroyShaderModule(geometry, nullptr, dl);
    }
    if (tessellated) {
        d.device.destroyShaderModule(control, nullptr, dl);
        d.device.destroyShaderModule(evaluation, nullptr, dl);
    }
    if (pipeline_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: no pipeline: {}", vk::to_string(pipeline_result));
        return result;
    }
    result.pipeline = pipeline;
    return result;
}

Pipeline CreateComputePipeline(const Device& d, std::span<const u32> code, u32 push_constant_bytes,
                               std::span<const vk::DescriptorSetLayout> set_layouts) {
    const auto& dl = d.dispatch;
    Pipeline result;
    const vk::PushConstantRange push{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .size = push_constant_bytes,
    };
    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = static_cast<u32>(set_layouts.size()),
        .pSetLayouts = set_layouts.data(),
        .pushConstantRangeCount = push_constant_bytes != 0 ? 1u : 0u,
        .pPushConstantRanges = &push,
    };
    result.layout = d.device.createPipelineLayout(layout_info, nullptr, dl).value;
    const vk::ShaderModule module = CreateModule(d, code);
    const vk::ComputePipelineCreateInfo info{
        .stage =
            {
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
        .layout = result.layout,
    };
    const auto [pipeline_result, pipeline] =
        d.device.createComputePipeline(d.pipeline_cache, info, nullptr, dl);
    d.device.destroyShaderModule(module, nullptr, dl);
    if (pipeline_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: no compute pipeline: {}",
                  vk::to_string(pipeline_result));
        return result;
    }
    result.pipeline = pipeline;
    return result;
}

void DestroyPipeline(const Device& d, Pipeline& pipeline) {
    d.device.destroyPipeline(pipeline.pipeline, nullptr, d.dispatch);
    d.device.destroyPipelineLayout(pipeline.layout, nullptr, d.dispatch);
    pipeline = {};
}

void LoadPipelineCache(Device& d) {
    const std::filesystem::path path = PipelineCachePath();
    std::vector<u8> data;
    const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (file.IsOpen()) {
        data.resize(file.GetSize());
        if (file.ReadRaw<u8>(data.data(), data.size()) != data.size()) {
            data.clear();
        }
    }
    // The driver ignores a cache that another driver or GPU wrote.
    const auto [result, cache] = d.device.createPipelineCache(
        {.initialDataSize = data.size(), .pInitialData = data.empty() ? nullptr : data.data()},
        nullptr, d.dispatch);
    if (result == vk::Result::eSuccess) {
        d.pipeline_cache = cache;
    }
    LOG_INFO(Render_Vulkan, "GR2port: pipeline cache of {} bytes from {}", data.size(),
             path.string());
}

void SavePipelineCache(const Device& d) {
    if (!d.pipeline_cache) {
        return;
    }
    std::scoped_lock lock{g_cache_file};
    const auto [result, data] = d.device.getPipelineCacheData(d.pipeline_cache, d.dispatch);
    if (result != vk::Result::eSuccess || data.empty()) {
        return;
    }
    // Written beside the last file and renamed over it, so that a run ending mid-write keeps it.
    const std::filesystem::path path = PipelineCachePath();
    std::filesystem::path written = path;
    written += ".new";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    {
        const Common::FS::IOFile file{written, Common::FS::FileAccessMode::Create};
        if (!file.IsOpen() || file.WriteRaw<u8>(data.data(), data.size()) != data.size()) {
            return;
        }
    }
    std::filesystem::rename(written, path, error);
    LOG_INFO(Render_Vulkan, "GR2port: pipeline cache of {} bytes saved{}", data.size(),
             error ? ": " + error.message() : "");
}

} // namespace Gr2Port
