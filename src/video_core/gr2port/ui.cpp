// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <span>
#include <unordered_map>

#include "common/logging/log.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/fullscreen_vert.h"
#include "video_core/gr2port/shaders/ui_blur_frag.h"
#include "video_core/gr2port/shaders/ui_blur_history_frag.h"
#include "video_core/gr2port/shaders/ui_color_control_frag.h"
#include "video_core/gr2port/shaders/ui_color_frag.h"
#include "video_core/gr2port/shaders/ui_copy_frag.h"
#include "video_core/gr2port/shaders/ui_frame_frag.h"
#include "video_core/gr2port/shaders/ui_gamma_frag.h"
#include "video_core/gr2port/shaders/ui_masked_frag.h"
#include "video_core/gr2port/shaders/ui_master_vert.h"
#include "video_core/gr2port/shaders/ui_movie_frag.h"
#include "video_core/gr2port/shaders/ui_pass_flat_vert.h"
#include "video_core/gr2port/shaders/ui_pass_vert.h"
#include "video_core/gr2port/shaders/ui_present_frag.h"
#include "video_core/gr2port/shaders/ui_skinned_b_vert.h"
#include "video_core/gr2port/shaders/ui_skinned_vert.h"
#include "video_core/gr2port/shaders/ui_sprite_frag.h"
#include "video_core/gr2port/shaders/ui_threshold_frag.h"
#include "video_core/gr2port/shaders/ui_transform_b_vert.h"
#include "video_core/gr2port/shaders/ui_transform_vert.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/ui.h"

namespace Gr2Port::Ui {

namespace {

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
// The game's samplers by their bits: bit 0 clamps, bit 1 filters by point.
constexpr u32 Samplers = 4;
constexpr u32 ClampSampler = 1;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
std::array<vk::Sampler, Samplers> g_samplers;
vk::DescriptorSetLayout g_sampler_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_sampler_set;
std::array<Image, Scene::MaxUiTargets> g_images;
std::unordered_map<u64, Pipeline> g_pipelines;
Pipeline g_present;
u32 g_open_target = Scene::MaxUiTargets;

void TargetBarrier(vk::CommandBuffer cmd, u32 target, vk::ImageLayout from, vk::ImageLayout to,
                   vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                   vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = g_images[target].image,
        .subresourceRange = ColorRange,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

// Targets rest in ShaderReadOnlyOptimal, where any draw may sample them.
void BeginTarget(vk::CommandBuffer cmd, u32 target) {
    TargetBarrier(
        cmd, target, vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite);
    const vk::RenderingAttachmentInfo color{
        .imageView = g_images[target].view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, g_images[target].extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color},
                       g_device->dispatch);
    g_open_target = target;
}

void EndTarget(vk::CommandBuffer cmd) {
    cmd.endRendering(g_device->dispatch);
    TargetBarrier(
        cmd, g_open_target, vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead);
    g_open_target = Scene::MaxUiTargets;
}

bool HasImage(const Scene::Header& header, u32 target) {
    return target < header.ui.targets && g_images[target].image;
}

vk::BlendFactor Factor(u32 value) {
    switch (value) {
    case 1:
        return vk::BlendFactor::eOne;
    case 2:
        return vk::BlendFactor::eSrcColor;
    case 3:
        return vk::BlendFactor::eOneMinusSrcColor;
    case 4:
        return vk::BlendFactor::eSrcAlpha;
    case 5:
        return vk::BlendFactor::eOneMinusSrcAlpha;
    case 6:
        return vk::BlendFactor::eDstAlpha;
    case 7:
        return vk::BlendFactor::eOneMinusDstAlpha;
    case 8:
        return vk::BlendFactor::eDstColor;
    case 9:
        return vk::BlendFactor::eOneMinusDstColor;
    case 10:
        return vk::BlendFactor::eSrcAlphaSaturate;
    case 13:
        return vk::BlendFactor::eConstantColor;
    case 14:
        return vk::BlendFactor::eOneMinusConstantColor;
    case 19:
        return vk::BlendFactor::eConstantAlpha;
    case 20:
        return vk::BlendFactor::eOneMinusConstantAlpha;
    default:
        return vk::BlendFactor::eZero;
    }
}

vk::BlendOp Operation(u32 value) {
    switch (value) {
    case 1:
        return vk::BlendOp::eSubtract;
    case 2:
        return vk::BlendOp::eMin;
    case 3:
        return vk::BlendOp::eMax;
    case 4:
        return vk::BlendOp::eReverseSubtract;
    default:
        return vk::BlendOp::eAdd;
    }
}

// CB_BLEND0_CONTROL: the colour's source factor, operation and destination factor from bit 0, the
// alpha's from bit 16, separate alpha at bit 29 and the enable at bit 30.
vk::PipelineColorBlendAttachmentState BlendState(u32 control) {
    const bool separate = (control >> 29 & 1) != 0;
    const u32 alpha = separate ? control >> 16 : control;
    return {
        .blendEnable = (control >> 30 & 1) != 0,
        .srcColorBlendFactor = Factor(control & 0x1f),
        .dstColorBlendFactor = Factor(control >> 8 & 0x1f),
        .colorBlendOp = Operation(control >> 5 & 7),
        .srcAlphaBlendFactor = Factor(alpha & 0x1f),
        .dstAlphaBlendFactor = Factor(alpha >> 8 & 0x1f),
        .alphaBlendOp = Operation(alpha >> 5 & 7),
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
}

std::span<const u32> VertexCode(u32 program) {
    switch (static_cast<Scene::UiVertexProgram>(program)) {
    case Scene::UiVertexProgram::Pass:
        return UI_PASS_VERT;
    case Scene::UiVertexProgram::PassFlat:
        return UI_PASS_FLAT_VERT;
    case Scene::UiVertexProgram::Master:
        return UI_MASTER_VERT;
    case Scene::UiVertexProgram::TransformB:
        return UI_TRANSFORM_B_VERT;
    case Scene::UiVertexProgram::Transform:
        return UI_TRANSFORM_VERT;
    case Scene::UiVertexProgram::Skinned:
        return UI_SKINNED_VERT;
    default:
        return UI_SKINNED_B_VERT;
    }
}

std::span<const u32> PixelCode(u32 program) {
    switch (static_cast<Scene::UiPixelProgram>(program)) {
    case Scene::UiPixelProgram::Color:
        return UI_COLOR_FRAG;
    case Scene::UiPixelProgram::Copy:
        return UI_COPY_FRAG;
    case Scene::UiPixelProgram::Gamma:
        return UI_GAMMA_FRAG;
    case Scene::UiPixelProgram::Threshold:
        return UI_THRESHOLD_FRAG;
    case Scene::UiPixelProgram::Movie:
        return UI_MOVIE_FRAG;
    case Scene::UiPixelProgram::Sprite:
        return UI_SPRITE_FRAG;
    case Scene::UiPixelProgram::Frame:
        return UI_FRAME_FRAG;
    case Scene::UiPixelProgram::Masked:
        return UI_MASKED_FRAG;
    case Scene::UiPixelProgram::Blur:
        return UI_BLUR_FRAG;
    case Scene::UiPixelProgram::BlurHistory:
        return UI_BLUR_HISTORY_FRAG;
    default:
        return UI_COLOR_CONTROL_FRAG;
    }
}

// A pipeline per pair of programs and blend, made when a frame first draws with them.
const Pipeline& PipelineFor(const Scene::UiRun& run) {
    const u64 key = u64{run.vertex} << 40 | u64{run.pixel} << 32 | run.blend;
    const auto [entry, added] = g_pipelines.try_emplace(key);
    if (added) {
        const vk::PipelineColorBlendAttachmentState blend = BlendState(run.blend);
        const std::array sets{Textures::SetLayout(), g_sampler_layout};
        entry->second = CreateGraphicsPipeline(*g_device, {
                                                              .vertex = VertexCode(run.vertex),
                                                              .fragment = PixelCode(run.pixel),
                                                              .color = TargetFormat,
                                                              .blend = &blend,
                                                              .dynamic_topology = true,
                                                              .push_constant_bytes = sizeof(u64),
                                                              .set_layouts = sets,
                                                          });
        LOG_INFO(Render_Vulkan, "GR2port UI: pipeline of programs {} {}, blend {:#x}: {}",
                 run.vertex, run.pixel, run.blend, entry->second.pipeline ? "made" : "failed");
    }
    return entry->second;
}

} // namespace

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    const auto& dl = device.dispatch;
    for (u32 i = 0; i < Samplers; i++) {
        const auto filter = (i & 2) != 0 ? vk::Filter::eNearest : vk::Filter::eLinear;
        const auto address = (i & ClampSampler) != 0 ? vk::SamplerAddressMode::eClampToEdge
                                                     : vk::SamplerAddressMode::eRepeat;
        g_samplers[i] = device.device
                            .createSampler(
                                {
                                    .magFilter = filter,
                                    .minFilter = filter,
                                    .mipmapMode = vk::SamplerMipmapMode::eLinear,
                                    .addressModeU = address,
                                    .addressModeV = address,
                                    .addressModeW = address,
                                    .maxLod = VK_LOD_CLAMP_NONE,
                                },
                                nullptr, dl)
                            .value;
        if (!g_samplers[i]) {
            return false;
        }
    }
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eSampler, Samplers,
                                                 vk::ShaderStageFlagBits::eFragment,
                                                 g_samplers.data()};
    g_sampler_layout =
        device.device
            .createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding}, nullptr, dl)
            .value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eSampler, Samplers};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size},
                                       nullptr, dl)
                 .value;
    if (!g_sampler_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_sampler_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_sampler_set = sets.front();
    return true;
}

bool Resize(vk::Format window_format) {
    DestroyPipeline(*g_device, g_present);
    const std::array sets{Textures::SetLayout(), g_sampler_layout};
    g_present = CreateGraphicsPipeline(*g_device, {
                                                      .vertex = FULLSCREEN_VERT,
                                                      .fragment = UI_PRESENT_FRAG,
                                                      .color = window_format,
                                                      .push_constant_bytes = sizeof(u32),
                                                      .set_layouts = sets,
                                                  });
    return static_cast<bool>(g_present.pipeline);
}

void Prepare(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 frame) {
    const auto& dl = g_device->dispatch;
    std::array<u32, Scene::MaxUiDynamicTextures> dynamic;
    const u32 dynamic_count = std::min(header.ui.dynamic, Scene::MaxUiDynamicTextures);
    std::memcpy(dynamic.data(), packet + Scene::UiDynamicOffset, dynamic_count * sizeof(u32));
    Textures::Refresh(cmd, frame, std::span{dynamic}.first(dynamic_count));

    std::array<Scene::UiTarget, Scene::MaxUiTargets> targets;
    const u32 count = std::min(header.ui.targets, Scene::MaxUiTargets);
    std::memcpy(targets.data(), packet + Scene::UiTargetsOffset, count * sizeof(Scene::UiTarget));
    for (u32 i = 0; i < count; i++) {
        if (g_images[i].image) {
            continue;
        }
        g_images[i] = g_memory->CreateImage(
            TargetFormat, {targets[i].width, targets[i].height},
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
            vk::ImageAspectFlagBits::eColor);
        if (!g_images[i].image) {
            LOG_ERROR(Render_Vulkan, "GR2port UI: no image for target {}, {}x{}", i,
                      targets[i].width, targets[i].height);
            continue;
        }
        TargetBarrier(cmd, i, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                      vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                      vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite);
        cmd.clearColorImage(g_images[i].image, vk::ImageLayout::eTransferDstOptimal,
                            vk::ClearColorValue{.float32 = std::array<float, 4>{0, 0, 0, 1}},
                            ColorRange, dl);
        TargetBarrier(
            cmd, i, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead);
        Textures::SetExternal(Scene::UiTargetHandles + i, g_images[i].view);
    }
}

vk::Extent2D BeginWorld(vk::CommandBuffer cmd, const Scene::Header& header) {
    if (!HasImage(header, header.ui.world)) {
        return {};
    }
    BeginTarget(cmd, header.ui.world);
    return g_images[header.ui.world].extent;
}

void EndWorld(vk::CommandBuffer cmd) {
    EndTarget(cmd);
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
            vk::Buffer buffer, u64 offset) {
    const auto& dl = g_device->dispatch;
    const u32 count = std::min(header.ui.runs, Scene::MaxUiRuns);
    if (count == 0) {
        return;
    }
    std::array<Scene::UiRun, Scene::MaxUiRuns> runs;
    std::memcpy(runs.data(), packet + Scene::UiRunsOffset, count * sizeof(Scene::UiRun));
    const u64 draws = address + Scene::UiDrawsOffset;
    const std::array sets{Textures::Set(), g_sampler_set};
    for (u32 i = 0; i < count; i++) {
        const Scene::UiRun& run = runs[i];
        if (!HasImage(header, run.target)) {
            continue;
        }
        const Pipeline& pipeline = PipelineFor(run);
        if (!pipeline.pipeline) {
            continue;
        }
        if (g_open_target != run.target) {
            if (g_open_target != Scene::MaxUiTargets) {
                EndTarget(cmd);
            }
            BeginTarget(cmd, run.target);
        }
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {}, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(draws),
                          &draws, dl);
        cmd.setPrimitiveTopology(run.topology == Scene::UiPrimitiveStrip
                                     ? vk::PrimitiveTopology::eTriangleStrip
                                     : vk::PrimitiveTopology::eTriangleList,
                                 dl);
        const vk::Viewport viewport{
            .x = run.viewport[0],
            .y = run.viewport[1],
            .width = run.viewport[2],
            .height = run.viewport[3],
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        cmd.setViewport(0, viewport, dl);
        cmd.setScissor(0, vk::Rect2D{{0, 0}, g_images[run.target].extent}, dl);
        cmd.drawIndirect(buffer,
                         offset + Scene::UiCommandsOffset +
                             u64{run.first} * sizeof(Scene::VertexCommand),
                         run.count, sizeof(Scene::VertexCommand), dl);
    }
    if (g_open_target != Scene::MaxUiTargets) {
        EndTarget(cmd);
    }
}

void RecordPresent(vk::CommandBuffer cmd, const Scene::Header& header, vk::Extent2D extent) {
    const auto& dl = g_device->dispatch;
    if (!HasImage(header, header.ui.picture)) {
        return;
    }
    const u32 picture = Scene::UiTargetHandles + header.ui.picture;
    const vk::Viewport viewport{
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .maxDepth = 1.0f,
    };
    const std::array sets{Textures::Set(), g_sampler_set};
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_present.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_present.layout, 0, sets, {}, dl);
    cmd.pushConstants(g_present.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(picture),
                      &picture, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    cmd.draw(3, 1, 0, 0, dl);
}

} // namespace Gr2Port::Ui
