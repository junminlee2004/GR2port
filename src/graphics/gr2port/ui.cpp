// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <unordered_map>

#include "common/logging/log.h"
#include "core/game_check/game_patches.h"
#include "graphics/gr2port/marker_fit.h"
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
#include "graphics/gr2port/shaders/ui_present_hdr_frag.h"
#include "graphics/gr2port/shaders/ui_skinned_b_vert.h"
#include "graphics/gr2port/shaders/ui_skinned_vert.h"
#include "graphics/gr2port/shaders/ui_sprite_frag.h"
#include "graphics/gr2port/shaders/ui_sprite_matrix_frag.h"
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
constexpr std::array<u32, PixelPrograms> PixelInputs{1, 2, 1, 2, 1, 3, 3, 3,
                                                     2, 2, 2, 2, 1, 3, 3, 3};
constexpr u32 StrokeVertex = static_cast<u32>(Scene::UiVertexProgram::Stroke);
constexpr u32 StrokePixel = static_cast<u32>(Scene::UiPixelProgram::Stroke);
std::array<Pipeline, VertexPrograms * PixelPrograms> g_pairs;
Pipeline g_present;
// With an HDR window, its paper white in units of 10000 nits, else 0.
float g_hdr_white = 0.0f;
// The push descriptor set of the gain image the HDR present reads.
vk::DescriptorSetLayout g_gain_layout;
// The gain images of the targets the world went into as the back buffer.
std::array<Image, Scene::MaxUiTargets> g_gains;
// The copy of the picture the photo filter's view reads, the size of the picture.
Image g_copy;
// This frame's world went into the back buffer with its gain image.
bool g_world_gain = false;
u32 g_open_target = Scene::MaxUiTargets;

struct HdrPresentPush {
    u32 picture;
    u32 has_world;
    float white;
};
// The shape of the screen the game lays its screen drawing out for.
constexpr float LayoutAspect = 16.0f / 9.0f;
// The pipelines run ui_text.glsl: the chosen aspect is not 16:9, so the game stretches its screen
// drawing.
bool g_narrow_text = false;
// With g_narrow_text: the chosen aspect keeps the whole screen drawing at its 16:9 shape.
bool g_fit_ui = false;

// The draws' device address, then ui_text.glsl's scales, the target's size and the viewport.
struct Push {
    u64 draws;
    std::array<float, 2> text_scale;
    std::array<float, 2> fit_scale;
    std::array<float, 2> target_size;
    std::array<float, 4> viewport;
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

// The scale of a run's text and the scale it fits the screen drawing by: with g_fit_ui the screen
// drawing fits by TextScale and text narrows no further.
std::array<std::array<float, 2>, 2> RunScales(vk::Extent2D screen, vk::Extent2D target) {
    const std::array<float, 2> scale = TextScale(screen, target);
    if (g_fit_ui) {
        return {std::array{1.0f, 1.0f}, scale};
    }
    return {scale, std::array{1.0f, 1.0f}};
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
// If gain is not null, it is the rendering's second attachment.
void BeginTarget(vk::CommandBuffer cmd, u32 target, vk::ImageView gain = {}) {
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
    const vk::RenderingAttachmentInfo gain_color{
        .imageView = gain,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const std::array colors{color, gain_color};
    cmd.beginRendering({.renderArea = {{0, 0}, g_images[target].extent},
                        .layerCount = 1,
                        .colorAttachmentCount = gain ? 2u : 1u,
                        .pColorAttachments = colors.data()},
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

// Whether the target has its gain image, which this makes on first use.
bool GainImage(u32 target) {
    if (!g_gains[target].image) {
        g_gains[target] = g_memory->CreateImage(GainFormat, g_images[target].extent,
                                                vk::ImageUsageFlagBits::eColorAttachment |
                                                    vk::ImageUsageFlagBits::eSampled,
                                                vk::ImageAspectFlagBits::eColor);
        if (!g_gains[target].image) {
            LOG_ERROR(Render_Vulkan, "GR2port UI: no gain image for target {}", target);
        }
    }
    return static_cast<bool>(g_gains[target].image);
}

bool HasImage(const Scene::Header& header, u32 target) {
    return target < header.ui.targets && g_images[target].image;
}

// Outside rendering: copies the target into g_copy, which this makes on first use.
bool CopyPicture(vk::CommandBuffer cmd, u32 target) {
    const vk::Extent2D extent = g_images[target].extent;
    if (!g_copy.image) {
        g_copy = g_memory->CreateImage(TargetFormat, extent,
                                       vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferDst,
                                       vk::ImageAspectFlagBits::eColor);
        if (!g_copy.image) {
            LOG_ERROR(Render_Vulkan, "GR2port UI: no image for the picture's copy");
            return false;
        }
        Textures::SetExternal(Scene::UiCopyHandle, g_copy.view);
    }
    if (g_copy.extent != extent) {
        return false;
    }
    TargetBarrier(cmd, target, vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::ImageLayout::eTransferSrcOptimal,
                  vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                  vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eTransferRead);
    ImageBarrier(*g_device, cmd, g_copy.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    cmd.copyImage(g_images[target].image, vk::ImageLayout::eTransferSrcOptimal, g_copy.image,
                  vk::ImageLayout::eTransferDstOptimal, region, g_device->dispatch);
    TargetBarrier(cmd, target, vk::ImageLayout::eTransferSrcOptimal,
                  vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                  vk::AccessFlagBits2::eTransferRead, vk::PipelineStageFlagBits2::eFragmentShader,
                  vk::AccessFlagBits2::eShaderSampledRead);
    ImageBarrier(*g_device, cmd, g_copy.image, vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eShaderSampledRead);
    return true;
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

// A screen draw's blend over the world in an HDR picture. The alpha holds the share of the world
// the draws leave, which the present gives the world's gain: none under an opaque draw, and under a
// draw that multiplies the world the share stays, as the colour it leaves caps it.
vk::PipelineColorBlendAttachmentState WorldBlendState(u32 control) {
    vk::PipelineColorBlendAttachmentState blend = BlendState(control);
    if (!blend.blendEnable) {
        blend.blendEnable = true;
        blend.srcColorBlendFactor = vk::BlendFactor::eOne;
        blend.dstColorBlendFactor = vk::BlendFactor::eZero;
        blend.colorBlendOp = vk::BlendOp::eAdd;
    }
    blend.srcAlphaBlendFactor = vk::BlendFactor::eZero;
    blend.dstAlphaBlendFactor = blend.srcColorBlendFactor == vk::BlendFactor::eDstColor
                                    ? vk::BlendFactor::eOne
                                    : blend.dstColorBlendFactor;
    blend.alphaBlendOp = vk::BlendOp::eAdd;
    return blend;
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
    case Scene::UiPixelProgram::SpriteMatrix:
        return UI_SPRITE_MATRIX_FRAG;
    default:
        return UI_COLOR_CONTROL_FRAG;
    }
}

// A pipeline per pair of programs and blend, made when a frame first draws with them.
const Pipeline& PipelineFor(const Scene::UiRun& run, bool over_world) {
    const u64 key =
        u64{over_world} << 48 | u64{run.vertex} << 40 | u64{run.pixel} << 32 | run.blend;
    const auto [entry, added] = g_pipelines.try_emplace(key);
    if (added) {
        const vk::PipelineColorBlendAttachmentState blend =
            over_world ? WorldBlendState(run.blend) : BlendState(run.blend);
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

// Whether one of run's draws in packet copies a movie's frame over the picture.
bool HasMovie(const u8* packet, const Scene::UiRun& run) {
    const u32 end = std::min(run.first + run.count, Scene::MaxUiDraws);
    for (u32 i = run.first; i < end; i++) {
        u32 flags;
        std::memcpy(&flags,
                    packet + Scene::UiDrawsOffset + u64{i} * sizeof(Scene::UiDraw) +
                        offsetof(Scene::UiDraw, flags),
                    sizeof(flags));
        if ((flags & Scene::UiDrawMovie) != 0) {
            return true;
        }
    }
    return false;
}

// Clears to black the open target's area outside its middle at fit_scale of its size: beside a
// movie's frame, which the geometry program fits there.
void ClearSides(vk::CommandBuffer cmd, vk::Extent2D target, std::array<float, 2> fit_scale) {
    // The sides' inner edges, rounded outward: the frame draws over any overlap.
    const auto side = [](u32 size, float scale) {
        return static_cast<u32>(std::ceil(0.5f * static_cast<float>(size) * (1.0f - scale)));
    };
    const u32 x = std::min(side(target.width, fit_scale[0]), target.width / 2);
    const u32 y = std::min(side(target.height, fit_scale[1]), target.height / 2);
    std::array<vk::ClearRect, 4> rects;
    u32 count = 0;
    const auto add = [&](u32 left, u32 top, u32 width, u32 height) {
        if (width != 0 && height != 0) {
            rects[count++] = {
                .rect = {{static_cast<s32>(left), static_cast<s32>(top)}, {width, height}},
                .layerCount = 1,
            };
        }
    };
    add(0, 0, x, target.height);
    add(target.width - x, 0, x, target.height);
    add(x, 0, target.width - 2 * x, y);
    add(x, target.height - y, target.width - 2 * x, y);
    if (count == 0) {
        return;
    }
    const vk::ClearAttachment black{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .colorAttachment = 0,
        .clearValue = {.color = vk::ClearColorValue{.float32 = std::array{0.0f, 0.0f, 0.0f, 1.0f}}},
    };
    cmd.clearAttachments(black, std::span{rects.data(), count}, g_device->dispatch);
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
    g_sampler_layout = CreateSetLayout(device, std::span{&binding, 1});
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
    const vk::DescriptorSetLayoutBinding gain_binding{0, vk::DescriptorType::eSampledImage, 1,
                                                      vk::ShaderStageFlagBits::eFragment};
    g_gain_layout = device.device
                        .createDescriptorSetLayout(
                            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                             .bindingCount = 1,
                             .pBindings = &gain_binding},
                            nullptr, dl)
                        .value;
    if (!g_gain_layout) {
        return false;
    }
    const float aspect = GamePatches::AspectRatio();
    if (std::abs(aspect - LayoutAspect) > 0.01f) {
        g_narrow_text = device.geometry_shaders;
        g_fit_ui = g_narrow_text && GamePatches::FitsUi();
        if (g_fit_ui) {
            LOG_INFO(Render_Vulkan,
                     "GR2port UI: the screen drawing keeps its 16:9 shape in the middle of a "
                     "{:.3f} screen",
                     aspect);
        } else if (g_narrow_text) {
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
    PipelineBatch batch{device};
    u32 made = 0;
    for (u32 vertex = 0; vertex < VertexPrograms; vertex++) {
        for (u32 pixel = 0; pixel < PixelPrograms; pixel++) {
            if (!Pairs(vertex, pixel) || (vertex == StrokeVertex && !device.geometry_shaders)) {
                continue;
            }
            batch.Add(g_pairs[vertex * PixelPrograms + pixel],
                      {
                          .vertex = VertexCode(vertex),
                          .geometry = GeometryCode(vertex),
                          .fragment = PixelCode(pixel),
                          .color = TargetFormat,
                          .dynamic_topology = true,
                          .dynamic_blend = true,
                          .push_constant_bytes = sizeof(Push),
                          .set_layouts = pipeline_sets,
                      });
            made++;
        }
    }
    if (!batch.Build()) {
        return false;
    }
    LOG_INFO(Render_Vulkan, "GR2port UI: {} pipelines made at start", made);
    return true;
}

bool FitsScreen() {
    return g_fit_ui;
}

bool Resize(vk::Format window_format, float hdr_white) {
    DestroyPipeline(*g_device, g_present);
    g_hdr_white = hdr_white;
    const bool hdr = hdr_white > 0.0f;
    const std::array sets{Textures::SetLayout(), g_sampler_layout, g_gain_layout};
    g_present = CreateGraphicsPipeline(
        *g_device, {
                       .vertex = FULLSCREEN_VERT,
                       .fragment = hdr ? std::span<const u32>{UI_PRESENT_HDR_FRAG}
                                       : std::span<const u32>{UI_PRESENT_FRAG},
                       .color = window_format,
                       .push_constant_bytes = hdr ? u32{sizeof(HdrPresentPush)} : u32{sizeof(u32)},
                       .set_layouts = std::span{sets}.first(hdr ? 3 : 2),
                   });
    return static_cast<bool>(g_present.pipeline);
}

void Prepare(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 frame) {
    const auto& dl = g_device->dispatch;
    g_world_gain = false;
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

vk::Extent2D BeginWorld(vk::CommandBuffer cmd, const Scene::Header& header, bool hdr) {
    const u32 world = header.ui.world;
    if (!HasImage(header, world)) {
        return {};
    }
    g_world_gain = hdr && world == header.ui.picture && GainImage(world);
    if (g_world_gain) {
        // The last frame's present read it.
        ImageBarrier(*g_device, cmd, g_gains[world].image, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eColorAttachmentOptimal,
                     vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentWrite);
    }
    BeginTarget(cmd, world, g_world_gain ? g_gains[world].view : vk::ImageView{});
    return g_images[world].extent;
}

void EndWorld(vk::CommandBuffer cmd) {
    const u32 world = g_open_target;
    EndTarget(cmd);
    if (g_world_gain) {
        ImageBarrier(*g_device, cmd, g_gains[world].image, vk::ImageLayout::eColorAttachmentOptimal,
                     vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                     vk::AccessFlagBits2::eColorAttachmentWrite,
                     vk::PipelineStageFlagBits2::eFragmentShader,
                     vk::AccessFlagBits2::eShaderSampledRead);
    }
}

bool WorldGain() {
    return g_world_gain;
}

vk::Extent2D TargetExtent(u32 target) {
    return target < g_images.size() && g_images[target].image ? g_images[target].extent
                                                              : vk::Extent2D{};
}

vk::Image TargetImage(u32 target) {
    return target < g_images.size() ? g_images[target].image : vk::Image{};
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
    if (g_fit_ui && screen.height != 0) {
        MarkerFit::SetFitScale(TextScale(screen, screen));
    }
    const std::array sets{Textures::Set(), g_sampler_set};
    for (u32 i = 0; i < count; i++) {
        const Scene::UiRun& run = runs[i];
        if (!HasImage(header, run.target) ||
            (header.ui.hidden != 0 && run.kept == 0 && run.target == header.ui.picture)) {
            continue;
        }
        if (run.vertex >= VertexPrograms || run.pixel >= PixelPrograms ||
            !Pairs(run.vertex, run.pixel) ||
            (run.vertex == StrokeVertex && !g_device->geometry_shaders)) {
            continue;
        }
        const bool over_world = g_world_gain && run.target == header.ui.picture;
        const Pipeline& pipeline = g_device->dynamic_blend
                                       ? g_pairs[run.vertex * PixelPrograms + run.pixel]
                                       : PipelineFor(run, over_world);
        if (!pipeline.pipeline) {
            continue;
        }
        if (i + 1 == header.ui.copy) {
            if (g_open_target != Scene::MaxUiTargets) {
                EndTarget(cmd);
            }
            if (!CopyPicture(cmd, run.target)) {
                continue;
            }
        }
        if (g_open_target != run.target) {
            if (g_open_target != Scene::MaxUiTargets) {
                EndTarget(cmd);
            }
            BeginTarget(cmd, run.target);
        }
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
        if (g_device->dynamic_blend) {
            const vk::PipelineColorBlendAttachmentState blend =
                over_world ? WorldBlendState(run.blend) : BlendState(run.blend);
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
        const vk::Extent2D target = g_images[run.target].extent;
        const auto [text_scale, fit_scale] = RunScales(screen, target);
        const Push push{
            .draws = draws,
            .text_scale = text_scale,
            .fit_scale = fit_scale,
            .target_size = {static_cast<float>(target.width), static_cast<float>(target.height)},
            .viewport = {run.viewport[0], run.viewport[1], run.viewport[2], run.viewport[3]},
        };
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        vk::Viewport viewport{
            .x = run.viewport[0],
            .y = run.viewport[1],
            .width = run.viewport[2],
            .height = run.viewport[3],
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        // The strokes' own geometry program does not fit them, so their viewport narrows around
        // the target's middle instead.
        if (run.vertex == StrokeVertex) {
            const float middle_x = 0.5f * static_cast<float>(target.width);
            const float middle_y = 0.5f * static_cast<float>(target.height);
            viewport.x = middle_x + (viewport.x - middle_x) * fit_scale[0];
            viewport.width *= fit_scale[0];
            viewport.y = middle_y + (viewport.y - middle_y) * fit_scale[1];
            viewport.height *= fit_scale[1];
        }
        cmd.setViewport(0, viewport, dl);
        cmd.setScissor(0, vk::Rect2D{{0, 0}, g_images[run.target].extent}, dl);
        // At 16:9 a movie's frame covers the whole picture, so beside the fitted frame the picture
        // would keep what the game drew before it, such as the game's last frame.
        if ((fit_scale[0] < 1.0f || fit_scale[1] < 1.0f) && HasMovie(packet, run)) {
            ClearSides(cmd, target, fit_scale);
        }
        cmd.drawIndirect(buffer,
                         offset + Scene::UiCommandsOffset +
                             u64{run.first} * sizeof(Scene::VertexCommand),
                         run.count, sizeof(Scene::VertexCommand), dl);
    }
    if (g_open_target != Scene::MaxUiTargets) {
        EndTarget(cmd);
    }
}

void RecordPresent(vk::CommandBuffer cmd, const Scene::Header& header, vk::Extent2D extent,
                   bool generated) {
    const auto& dl = g_device->dispatch;
    if (!HasImage(header, header.ui.picture) || extent.width == 0 || extent.height == 0) {
        return;
    }
    const u32 picture =
        generated ? Scene::GeneratedHandle : Scene::UiTargetHandles + header.ui.picture;
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
    if (g_hdr_white > 0.0f) {
        // Without the world in the back buffer the set takes the back buffer, which goes unread.
        const vk::DescriptorImageInfo world{
            .imageView =
                g_world_gain ? g_gains[header.ui.picture].view : g_images[header.ui.picture].view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const vk::WriteDescriptorSet write{
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &world,
        };
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, g_present.layout, 2, write, dl);
        const HdrPresentPush push{
            .picture = picture,
            .has_world = g_world_gain ? 1u : 0u,
            .white = g_hdr_white,
        };
        cmd.pushConstants(g_present.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
    } else {
        cmd.pushConstants(g_present.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                          sizeof(picture), &picture, dl);
    }
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, area, dl);
    cmd.draw(3, 1, 0, 0, dl);
}

} // namespace Gr2Port::Ui
