// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <unordered_map>

#include "common/logging/log.h"
#include "core/game_check/game_patches.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/shaders/fullscreen_vert.h"
#include "graphics/gr2port/shaders/ui_blur_frag.h"
#include "graphics/gr2port/shaders/ui_blur_history_frag.h"
#include "graphics/gr2port/shaders/ui_color_control_frag.h"
#include "graphics/gr2port/shaders/ui_color_frag.h"
#include "graphics/gr2port/shaders/ui_copy_frag.h"
#include "graphics/gr2port/shaders/ui_frame_frag.h"
#include "graphics/gr2port/shaders/ui_gamma_frag.h"
#include "graphics/gr2port/shaders/ui_mask_frag.h"
#include "graphics/gr2port/shaders/ui_mask_sprite_frag.h"
#include "graphics/gr2port/shaders/ui_masked_frag.h"
#include "graphics/gr2port/shaders/ui_master_vert.h"
#include "graphics/gr2port/shaders/ui_movie_frag.h"
#include "graphics/gr2port/shaders/ui_pass_flat_vert.h"
#include "graphics/gr2port/shaders/ui_pass_vert.h"
#include "graphics/gr2port/shaders/ui_photo_frag.h"
#include "graphics/gr2port/shaders/ui_photo_vert.h"
#include "graphics/gr2port/shaders/ui_present_frag.h"
#include "graphics/gr2port/shaders/ui_skinned_b_vert.h"
#include "graphics/gr2port/shaders/ui_skinned_vert.h"
#include "graphics/gr2port/shaders/ui_sprite_frag.h"
#include "graphics/gr2port/shaders/ui_stroke_frag.h"
#include "graphics/gr2port/shaders/ui_stroke_geom.h"
#include "graphics/gr2port/shaders/ui_stroke_vert.h"
#include "graphics/gr2port/shaders/ui_text_flat_geom.h"
#include "graphics/gr2port/shaders/ui_text_geom.h"
#include "graphics/gr2port/shaders/ui_threshold_frag.h"
#include "graphics/gr2port/shaders/ui_transform_b_vert.h"
#include "graphics/gr2port/shaders/ui_transform_vert.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/ui.h"

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
// With dynamic blend, a pipeline for each pair of programs whose interfaces match, made at start:
// the vertex programs write locations 0 and 1 but PassFlat only 0, and each pixel program reads
// the locations in its mask. The strokes' programs pair only with each other.
constexpr u32 VertexPrograms = static_cast<u32>(Scene::UiVertexProgram::Count);
constexpr u32 PixelPrograms = static_cast<u32>(Scene::UiPixelProgram::Count);
constexpr std::array<u32, VertexPrograms> VertexOutputs{3, 1, 3, 3, 3, 3, 3, 3, 3};
constexpr std::array<u32, PixelPrograms> PixelInputs{1, 2, 1, 2, 1, 3, 3, 3, 2, 2, 2, 2, 1, 3, 3};
constexpr u32 StrokeVertex = static_cast<u32>(Scene::UiVertexProgram::Stroke);
constexpr u32 StrokePixel = static_cast<u32>(Scene::UiPixelProgram::Stroke);
std::array<Pipeline, VertexPrograms * PixelPrograms> g_pairs;
Pipeline g_present;
u32 g_open_target = Scene::MaxUiTargets;
// The shape of the screen the game lays its screen drawing out for.
constexpr float LayoutAspect = 16.0f / 9.0f;
// The pipelines run ui_text.glsl: the chosen aspect is not 16:9, so the game stretches its screen
// drawing.
bool g_narrow_text = false;

// The draws' device address, then ui_text.glsl's scale.
struct Push {
    u64 draws;
    std::array<float, 2> text_scale;
};

bool Pairs(u32 vertex_program, u32 pixel_program) {
    return (PixelInputs[pixel_program] & ~VertexOutputs[vertex_program]) == 0 &&
           (vertex_program == StrokeVertex) == (pixel_program == StrokePixel);
}

std::span<const u32> GeometryCode(u32 vertex_program) {
    if (vertex_program == StrokeVertex) {
        return UI_STROKE_GEOM;
    }
    if (!g_narrow_text) {
        return {};
    }
    return VertexOutputs[vertex_program] == 1 ? std::span<const u32>{UI_TEXT_FLAT_GEOM}
                                              : std::span<const u32>{UI_TEXT_GEOM};
}

// The scale that gives text its 16:9 shape in a target with the back buffer's shape; 1 for
// targets of other shapes and for a 16:9 back buffer.
std::array<float, 2> TextScale(vk::Extent2D screen, vk::Extent2D target) {
    if (!g_narrow_text || screen.height == 0 || target.height == 0) {
        return {1.0f, 1.0f};
    }
    const float aspect = static_cast<float>(screen.width) / static_cast<float>(screen.height);
    const float shape = static_cast<float>(target.width) / static_cast<float>(target.height);
    if (std::abs(shape - aspect) > 0.01f) {
        return {1.0f, 1.0f};
    }
    return {std::min(1.0f, LayoutAspect / aspect), std::min(1.0f, aspect / LayoutAspect)};
}

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
    case Scene::UiVertexProgram::Photo:
        return UI_PHOTO_VERT;
    case Scene::UiVertexProgram::Stroke:
        return UI_STROKE_VERT;
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
    case Scene::UiPixelProgram::Photo:
        return UI_PHOTO_FRAG;
    case Scene::UiPixelProgram::Mask:
        return UI_MASK_FRAG;
    case Scene::UiPixelProgram::MaskSprite:
        return UI_MASK_SPRITE_FRAG;
    case Scene::UiPixelProgram::Stroke:
        return UI_STROKE_FRAG;
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
                                                              .geometry = GeometryCode(run.vertex),
                                                              .fragment = PixelCode(run.pixel),
                                                              .color = TargetFormat,
                                                              .blend = &blend,
                                                              .dynamic_topology = true,
                                                              .push_constant_bytes = sizeof(Push),
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
    const float aspect = GamePatches::AspectRatio();
    if (std::abs(aspect - LayoutAspect) > 0.01f) {
        g_narrow_text = device.geometry_shaders;
        if (g_narrow_text) {
            LOG_INFO(Render_Vulkan, "GR2port UI: text keeps its 16:9 shape on a {:.3f} screen",
                     aspect);
        } else {
            LOG_WARNING(Render_Vulkan, "GR2port UI: no geometry shaders; text stretches to {:.3f}",
                        aspect);
        }
    }
    if (!device.dynamic_blend) {
        LOG_WARNING(Render_Vulkan,
                    "GR2port UI: the device has no dynamic blend; pipelines are made on first use");
        return true;
    }
    const std::array pipeline_sets{Textures::SetLayout(), g_sampler_layout};
    u32 made = 0;
    for (u32 vertex = 0; vertex < VertexPrograms; vertex++) {
        for (u32 pixel = 0; pixel < PixelPrograms; pixel++) {
            if (!Pairs(vertex, pixel) || (vertex == StrokeVertex && !device.geometry_shaders)) {
                continue;
            }
            Pipeline& pipeline = g_pairs[vertex * PixelPrograms + pixel];
            pipeline = CreateGraphicsPipeline(device, {
                                                          .vertex = VertexCode(vertex),
                                                          .geometry = GeometryCode(vertex),
                                                          .fragment = PixelCode(pixel),
                                                          .color = TargetFormat,
                                                          .dynamic_topology = true,
                                                          .dynamic_blend = true,
                                                          .push_constant_bytes = sizeof(Push),
                                                          .set_layouts = pipeline_sets,
                                                      });
            if (!pipeline.pipeline) {
                return false;
            }
            made++;
        }
    }
    LOG_INFO(Render_Vulkan, "GR2port UI: {} pipelines made at start", made);
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

vk::Extent2D TargetExtent(u32 target) {
    return target < g_images.size() && g_images[target].image ? g_images[target].extent
                                                              : vk::Extent2D{};
}

void CopyTarget(vk::CommandBuffer cmd, u32 target, vk::Buffer buffer) {
    TargetBarrier(cmd, target, vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::ImageLayout::eTransferSrcOptimal,
                  vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                      vk::PipelineStageFlagBits2::eFragmentShader,
                  vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eTransferRead);
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {g_images[target].extent.width, g_images[target].extent.height, 1},
    };
    cmd.copyImageToBuffer(g_images[target].image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                          region, g_device->dispatch);
    TargetBarrier(cmd, target, vk::ImageLayout::eTransferSrcOptimal,
                  vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eFragmentShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
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
    const vk::Extent2D screen =
        HasImage(header, header.ui.picture) ? g_images[header.ui.picture].extent : vk::Extent2D{};
    const std::array sets{Textures::Set(), g_sampler_set};
    for (u32 i = 0; i < count; i++) {
        const Scene::UiRun& run = runs[i];
        if (!HasImage(header, run.target)) {
            continue;
        }
        if (run.vertex >= VertexPrograms || run.pixel >= PixelPrograms ||
            !Pairs(run.vertex, run.pixel) ||
            (run.vertex == StrokeVertex && !g_device->geometry_shaders)) {
            continue;
        }
        const Pipeline& pipeline = g_device->dynamic_blend
                                       ? g_pairs[run.vertex * PixelPrograms + run.pixel]
                                       : PipelineFor(run);
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
        if (g_device->dynamic_blend) {
            const vk::PipelineColorBlendAttachmentState blend = BlendState(run.blend);
            cmd.setColorBlendEnableEXT(0, blend.blendEnable, dl);
            cmd.setColorBlendEquationEXT(0,
                                         vk::ColorBlendEquationEXT{
                                             .srcColorBlendFactor = blend.srcColorBlendFactor,
                                             .dstColorBlendFactor = blend.dstColorBlendFactor,
                                             .colorBlendOp = blend.colorBlendOp,
                                             .srcAlphaBlendFactor = blend.srcAlphaBlendFactor,
                                             .dstAlphaBlendFactor = blend.dstAlphaBlendFactor,
                                             .alphaBlendOp = blend.alphaBlendOp,
                                         },
                                         dl);
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {}, dl);
        cmd.setPrimitiveTopology(run.topology == Scene::UiPrimitiveStrip
                                     ? vk::PrimitiveTopology::eTriangleStrip
                                     : vk::PrimitiveTopology::eTriangleList,
                                 dl);
        const Push push{.draws = draws,
                        .text_scale = TextScale(screen, g_images[run.target].extent)};
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
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
    if (!HasImage(header, header.ui.picture) || extent.width == 0 || extent.height == 0) {
        return;
    }
    const u32 picture = Scene::UiTargetHandles + header.ui.picture;
    // The back buffer keeps its shape, centred in the window.
    const vk::Extent2D source = g_images[header.ui.picture].extent;
    const float scale =
        std::min(static_cast<float>(extent.width) / static_cast<float>(source.width),
                 static_cast<float>(extent.height) / static_cast<float>(source.height));
    const vk::Extent2D fitted{
        std::clamp(static_cast<u32>(std::lround(scale * static_cast<float>(source.width))), 1u,
                   extent.width),
        std::clamp(static_cast<u32>(std::lround(scale * static_cast<float>(source.height))), 1u,
                   extent.height),
    };
    const vk::Rect2D area{{static_cast<s32>((extent.width - fitted.width) / 2),
                           static_cast<s32>((extent.height - fitted.height) / 2)},
                          fitted};
    const vk::Viewport viewport{
        .x = static_cast<float>(area.offset.x),
        .y = static_cast<float>(area.offset.y),
        .width = static_cast<float>(fitted.width),
        .height = static_cast<float>(fitted.height),
        .maxDepth = 1.0f,
    };
    const std::array sets{Textures::Set(), g_sampler_set};
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_present.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_present.layout, 0, sets, {}, dl);
    cmd.pushConstants(g_present.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(picture),
                      &picture, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, area, dl);
    cmd.draw(3, 1, 0, 0, dl);
}

} // namespace Gr2Port::Ui
