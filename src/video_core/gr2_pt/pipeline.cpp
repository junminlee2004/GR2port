// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "video_core/gr2_pt/pipeline.h"

namespace Gr2Pt {

namespace {

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
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vertex,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment,
            .pName = "main",
        },
    };
    const u32 stage_count = fragment ? 2 : 1;
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{.topology = info.topology};
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
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil{
        .depthTestEnable = info.depth_test,
        .depthWriteEnable = info.depth_test,
        .depthCompareOp = info.depth_compare,
    };
    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const u32 color_count = info.color != vk::Format::eUndefined ? 1 : 0;
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = color_count,
        .pAttachments = &blend_attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor,
                                    vk::DynamicState::eDepthBias};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()) - (info.depth_bias ? 0 : 1),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .colorAttachmentCount = color_count,
        .pColorAttachmentFormats = &info.color,
        .depthAttachmentFormat = info.depth,
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering,
        .stageCount = stage_count,
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &rasterization,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = result.layout,
    };
    const auto [pipeline_result, pipeline] =
        d.device.createGraphicsPipeline(nullptr, pipeline_info, nullptr, dl);
    d.device.destroyShaderModule(vertex, nullptr, dl);
    if (fragment) {
        d.device.destroyShaderModule(fragment, nullptr, dl);
    }
    if (pipeline_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no pipeline: {}", vk::to_string(pipeline_result));
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

} // namespace Gr2Pt
