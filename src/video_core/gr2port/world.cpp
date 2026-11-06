// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>

#include "common/logging/log.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/deferred_frag.h"
#include "video_core/gr2port/shaders/fullscreen_vert.h"
#include "video_core/gr2port/shaders/occlusion_comp.h"
#include "video_core/gr2port/shaders/occlusion_depth_comp.h"
#include "video_core/gr2port/shaders/world_alpha_frag.h"
#include "video_core/gr2port/shaders/world_frag.h"
#include "video_core/gr2port/shaders/world_lines_comp.h"
#include "video_core/gr2port/shaders/world_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/vegetation.h"
#include "video_core/gr2port/world.h"

namespace Gr2Port::World {

namespace {

struct SurfacePush {
    u64 draws;
    u64 materials;
};

// The light block, and the frame's place in the game's cycle of 26 frames.
struct FramePush {
    u64 light;
    s32 frame;
    u32 padding;
};

// The world's lines and its ambient occlusion: their value, the scaled distance, the samples
// averaged. Two images of each take turns as the frame's output and the last frame's history.
constexpr vk::Format LinesFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr u32 LineImages = 2;
constexpr s32 LineFrames = 26;
constexpr u32 LinesGroupPixels = 8;
// The occlusion's depth levels 1 to 5, as the mips of one half-size image.
constexpr vk::Format DepthLevelFormat = vk::Format::eR32Sfloat;
constexpr u32 DepthLevels = 5;
constexpr u32 DepthLevelMinSize = 1u << DepthLevels;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
std::array<Image, SurfaceFormats.size()> g_surfaces;
vk::Extent2D g_extent;
vk::Sampler g_sampler;
vk::Sampler g_linear_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorSetLayout g_lines_layout;
vk::DescriptorSetLayout g_occlusion_layout;
vk::DescriptorSetLayout g_depth_level_layout;
vk::DescriptorPool g_pool;
// Each indexed by the image that holds the frame's lines and occlusion.
std::array<vk::DescriptorSet, LineImages> g_sets;
std::array<vk::DescriptorSet, LineImages> g_lines_sets;
std::array<vk::DescriptorSet, LineImages> g_occlusion_sets;
std::array<Image, LineImages> g_lines;
std::array<Image, LineImages> g_occlusion;
bool g_lines_cleared = false;
u32 g_lines_image = 0;
s32 g_lines_frame = 0;
Image g_depth_levels;
std::array<vk::ImageView, DepthLevels> g_depth_level_views;
std::array<vk::DescriptorSet, DepthLevels> g_depth_level_sets;
// The surface pipelines, in the order of the draw lists.
std::array<Pipeline, 4> g_surface_pipelines;
Pipeline g_light;
Pipeline g_lines_pipeline;
Pipeline g_occlusion_pipeline;
Pipeline g_depth_level_pipeline;

vk::DescriptorSetLayout CreateLayout(const Device& d,
                                     std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    return d.device
        .createDescriptorSetLayout(
            {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
            nullptr, d.dispatch)
        .value;
}

template <size_t N>
bool AllocateSets(const Device& d, vk::DescriptorSetLayout layout,
                  std::array<vk::DescriptorSet, N>& sets) {
    std::array<vk::DescriptorSetLayout, N> layouts;
    layouts.fill(layout);
    const auto [result, allocated] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = N, .pSetLayouts = layouts.data()},
        d.dispatch);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    std::ranges::copy(allocated, sets.begin());
    return true;
}

bool CreateSet(const Device& d) {
    const auto& dl = d.dispatch;
    const vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_sampler = d.device.createSampler(sampler_info, nullptr, dl).value;
    vk::SamplerCreateInfo linear_info = sampler_info;
    linear_info.magFilter = vk::Filter::eLinear;
    linear_info.minFilter = vk::Filter::eLinear;
    g_linear_sampler = d.device.createSampler(linear_info, nullptr, dl).value;
    constexpr auto Combined = vk::DescriptorType::eCombinedImageSampler;
    constexpr auto Storage = vk::DescriptorType::eStorageImage;
    constexpr auto Compute = vk::ShaderStageFlagBits::eCompute;
    // Depth, the surfaces, the lines and the occlusion.
    std::array<vk::DescriptorSetLayoutBinding, 3 + SurfaceFormats.size()> bindings;
    for (u32 i = 0; i < bindings.size(); i++) {
        bindings[i] = {i, Combined, 1, vk::ShaderStageFlagBits::eFragment, &g_sampler};
    }
    const std::array<vk::DescriptorSetLayoutBinding, 4> line_bindings{{
        {0, Combined, 1, Compute, &g_sampler},
        {1, Combined, 1, Compute, &g_linear_sampler},
        {2, Combined, 1, Compute, &g_linear_sampler},
        {3, Storage, 1, Compute},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 6> occlusion_bindings{{
        {0, Combined, 1, Compute, &g_sampler},
        {1, Combined, 1, Compute, &g_sampler},
        {2, Combined, 1, Compute, &g_sampler},
        {3, Combined, 1, Compute, &g_linear_sampler},
        {4, Storage, 1, Compute},
        {5, Combined, 1, Compute, &g_sampler},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 3> depth_level_bindings{{
        {0, Combined, 1, Compute, &g_sampler},
        {1, Storage, 1, Compute},
        {2, Combined, 1, Compute, &g_sampler},
    }};
    g_set_layout = CreateLayout(d, bindings);
    g_lines_layout = CreateLayout(d, line_bindings);
    g_occlusion_layout = CreateLayout(d, occlusion_bindings);
    g_depth_level_layout = CreateLayout(d, depth_level_bindings);
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {Combined, LineImages * static_cast<u32>(bindings.size() + 3 + 5) + 2 * DepthLevels},
        {Storage, 2 * LineImages + DepthLevels},
    }};
    g_pool = d.device
                 .createDescriptorPool({.maxSets = 3 * LineImages + DepthLevels,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_sampler || !g_linear_sampler || !g_set_layout || !g_lines_layout ||
        !g_occlusion_layout || !g_depth_level_layout || !g_pool) {
        return false;
    }
    return AllocateSets(d, g_set_layout, g_sets) && AllocateSets(d, g_lines_layout, g_lines_sets) &&
           AllocateSets(d, g_occlusion_layout, g_occlusion_sets) &&
           AllocateSets(d, g_depth_level_layout, g_depth_level_sets);
}

// Makes a compute pass's image writes readable, and orders the next frames' writes after this
// frame's reads of the images.
void WrittenBarrier(vk::CommandBuffer cmd) {
    const vk::PipelineStageFlags2 stages =
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader;
    const vk::MemoryBarrier2 written{
        .srcStageMask = stages,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = stages,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written},
                         g_device->dispatch);
}

Pipeline CreateSurfacePipeline(const Device& d, vk::Format depth_format,
                               std::span<const u32> fragment, vk::CullModeFlags cull) {
    const std::array texture_set{Textures::SetLayout()};
    return CreateGraphicsPipeline(d, {
                                         .vertex = WORLD_VERT,
                                         .fragment = fragment,
                                         .colors = SurfaceFormats,
                                         .depth = depth_format,
                                         .depth_test = true,
                                         .cull = cull,
                                         .push_constant_bytes = sizeof(SurfacePush),
                                         .set_layouts = texture_set,
                                     });
}

} // namespace

bool Start(const Device& device, Memory& memory, vk::Format depth_format, vk::Format scene_format) {
    g_device = &device;
    g_memory = &memory;
    if (!CreateSet(device)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the surfaces' set failed");
        return false;
    }
    constexpr auto Back = vk::CullModeFlagBits::eBack;
    constexpr auto None = vk::CullModeFlagBits::eNone;
    g_surface_pipelines = {CreateSurfacePipeline(device, depth_format, WORLD_FRAG, Back),
                           CreateSurfacePipeline(device, depth_format, WORLD_FRAG, None),
                           CreateSurfacePipeline(device, depth_format, WORLD_ALPHA_FRAG, Back),
                           CreateSurfacePipeline(device, depth_format, WORLD_ALPHA_FRAG, None)};
    const std::array light_sets{Textures::SetLayout(), Shadows::SetLayout(), g_set_layout};
    g_light = CreateGraphicsPipeline(device, {
                                                 .vertex = FULLSCREEN_VERT,
                                                 .fragment = DEFERRED_FRAG,
                                                 .color = scene_format,
                                                 .push_constant_bytes = sizeof(u64),
                                                 .set_layouts = light_sets,
                                             });
    const std::array line_sets{g_lines_layout};
    g_lines_pipeline =
        CreateComputePipeline(device, WORLD_LINES_COMP, sizeof(FramePush), line_sets);
    const std::array occlusion_sets{g_occlusion_layout};
    g_occlusion_pipeline =
        CreateComputePipeline(device, OCCLUSION_COMP, sizeof(FramePush), occlusion_sets);
    const std::array depth_level_sets{g_depth_level_layout};
    g_depth_level_pipeline =
        CreateComputePipeline(device, OCCLUSION_DEPTH_COMP, sizeof(s32), depth_level_sets);
    return std::ranges::all_of(g_surface_pipelines,
                               [](const Pipeline& pipeline) { return !!pipeline.pipeline; }) &&
           g_light.pipeline && g_lines_pipeline.pipeline && g_occlusion_pipeline.pipeline &&
           g_depth_level_pipeline.pipeline;
}

bool Resize(vk::Extent2D extent, vk::ImageView depth, vk::ImageView character_depth) {
    g_extent = extent;
    for (u32 i = 0; i < SurfaceFormats.size(); i++) {
        g_memory->Destroy(g_surfaces[i]);
        g_surfaces[i] = g_memory->CreateImage(SurfaceFormats[i], extent,
                                              vk::ImageUsageFlagBits::eColorAttachment |
                                                  vk::ImageUsageFlagBits::eSampled,
                                              vk::ImageAspectFlagBits::eColor);
        if (!g_surfaces[i].image) {
            return false;
        }
    }
    const auto& dl = g_device->dispatch;
    for (auto* images : {&g_lines, &g_occlusion}) {
        for (Image& image : *images) {
            g_memory->Destroy(image);
            image = g_memory->CreateImage(LinesFormat, extent,
                                          vk::ImageUsageFlagBits::eStorage |
                                              vk::ImageUsageFlagBits::eSampled |
                                              vk::ImageUsageFlagBits::eTransferDst,
                                          vk::ImageAspectFlagBits::eColor);
            if (!image.image) {
                return false;
            }
        }
    }
    for (vk::ImageView& view : g_depth_level_views) {
        if (view) {
            g_device->device.destroyImageView(view, nullptr, dl);
            view = vk::ImageView{};
        }
    }
    g_memory->Destroy(g_depth_levels);
    // At least one texel in the top level.
    const vk::Extent2D level_extent{std::max(extent.width / 2, DepthLevelMinSize),
                                    std::max(extent.height / 2, DepthLevelMinSize)};
    g_depth_levels =
        g_memory->CreateImage(DepthLevelFormat, level_extent,
                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                              vk::ImageAspectFlagBits::eColor, 1, DepthLevels);
    if (!g_depth_levels.image) {
        return false;
    }
    for (u32 k = 0; k < DepthLevels; k++) {
        const vk::ImageViewCreateInfo info{
            .image = g_depth_levels.image,
            .viewType = vk::ImageViewType::e2D,
            .format = DepthLevelFormat,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, k, 1, 0, 1},
        };
        g_depth_level_views[k] = g_device->device.createImageView(info, nullptr, dl).value;
    }
    g_lines_cleared = false;
    const vk::DescriptorImageInfo depth_info{
        .imageView = depth,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo character_depth_info{
        .imageView = character_depth,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
    };
    const auto read_info = [](const Image& image, vk::ImageLayout layout) {
        return vk::DescriptorImageInfo{.imageView = image.view, .imageLayout = layout};
    };
    const auto write = [](vk::DescriptorSet set, u32 binding, vk::DescriptorType type,
                          const vk::DescriptorImageInfo* info) {
        return vk::WriteDescriptorSet{
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = info,
        };
    };
    constexpr auto Combined = vk::DescriptorType::eCombinedImageSampler;
    const auto surface_info = [&read_info](u32 i) {
        return read_info(g_surfaces[i], vk::ImageLayout::eShaderReadOnlyOptimal);
    };
    constexpr auto Storage = vk::DescriptorType::eStorageImage;
    const std::array surface_infos{surface_info(0), surface_info(1), surface_info(2)};
    const vk::DescriptorImageInfo levels_info =
        read_info(g_depth_levels, vk::ImageLayout::eGeneral);
    for (u32 i = 0; i < LineImages; i++) {
        const u32 last = (i + 1) % LineImages;
        const vk::DescriptorImageInfo output = read_info(g_lines[i], vk::ImageLayout::eGeneral);
        const vk::DescriptorImageInfo history = read_info(g_lines[last], vk::ImageLayout::eGeneral);
        const vk::DescriptorImageInfo occlusion =
            read_info(g_occlusion[i], vk::ImageLayout::eGeneral);
        const vk::DescriptorImageInfo occlusion_history =
            read_info(g_occlusion[last], vk::ImageLayout::eGeneral);
        const std::array<vk::WriteDescriptorSet, 16> writes{
            write(g_sets[i], 0, Combined, &depth_info),
            write(g_sets[i], 1, Combined, &surface_infos[0]),
            write(g_sets[i], 2, Combined, &surface_infos[1]),
            write(g_sets[i], 3, Combined, &surface_infos[2]),
            write(g_sets[i], 4, Combined, &output),
            write(g_sets[i], 5, Combined, &occlusion),
            write(g_lines_sets[i], 0, Combined, &depth_info),
            write(g_lines_sets[i], 1, Combined, &surface_infos[1]),
            write(g_lines_sets[i], 2, Combined, &history),
            write(g_lines_sets[i], 3, Storage, &output),
            write(g_occlusion_sets[i], 0, Combined, &depth_info),
            write(g_occlusion_sets[i], 1, Combined, &levels_info),
            write(g_occlusion_sets[i], 2, Combined, &surface_infos[1]),
            write(g_occlusion_sets[i], 3, Combined, &occlusion_history),
            write(g_occlusion_sets[i], 4, Storage, &occlusion),
            write(g_occlusion_sets[i], 5, Combined, &character_depth_info),
        };
        g_device->device.updateDescriptorSets(writes, {}, dl);
    }
    for (u32 k = 0; k < DepthLevels; k++) {
        const vk::DescriptorImageInfo level{.imageView = g_depth_level_views[k],
                                            .imageLayout = vk::ImageLayout::eGeneral};
        const std::array<vk::WriteDescriptorSet, 3> writes{
            write(g_depth_level_sets[k], 0, Combined, &depth_info),
            write(g_depth_level_sets[k], 1, Storage, &level),
            write(g_depth_level_sets[k], 2, Combined, &character_depth_info),
        };
        g_device->device.updateDescriptorSets(writes, {}, dl);
    }
    return true;
}

void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, bool drawn, u64 address,
                    vk::Buffer buffer, u64 offset, vk::ImageView depth) {
    const auto& dl = g_device->dispatch;
    std::array<vk::ImageMemoryBarrier2, SurfaceFormats.size()> barriers;
    std::array<vk::RenderingAttachmentInfo, SurfaceFormats.size()> colors;
    for (u32 i = 0; i < SurfaceFormats.size(); i++) {
        // The last frame's light pass sampled the image.
        barriers[i] = {
            .srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
            .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .image = g_surfaces[i].image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        colors[i] = {
            .imageView = g_surfaces[i].view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = {.color =
                               vk::ClearColorValue{
                                   .float32 = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}}},
        };
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         dl);
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth,
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = {.depthStencil = {.depth = 0.0f}},
    };
    cmd.beginRendering({.renderArea = {{0, 0}, g_extent},
                        .layerCount = 1,
                        .colorAttachmentCount = static_cast<u32>(colors.size()),
                        .pColorAttachments = colors.data(),
                        .pDepthAttachment = &depth_attachment},
                       dl);
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport{
        .x = 0.0f,
        .y = static_cast<float>(g_extent.height),
        .width = static_cast<float>(g_extent.width),
        .height = -static_cast<float>(g_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, g_extent}, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    const SurfacePush push{
        .draws = address + Scene::DrawsOffset,
        .materials = address + Scene::MaterialsOffset,
    };
    u64 first = 0;
    for (u32 list = 0; drawn && list < g_surface_pipelines.size(); list++) {
        const u32 count = header.draw_commands[list];
        if (count != 0) {
            const Pipeline& pipeline = g_surface_pipelines[list];
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0,
                                   Textures::Set(), {}, dl);
            cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                              sizeof(push), &push, dl);
            cmd.drawIndexedIndirect(buffer,
                                    offset + Scene::CommandsOffset + first * sizeof(Scene::Command),
                                    count, sizeof(Scene::Command), dl);
        }
        first += count;
    }
    if (drawn) {
        Vegetation::RecordSurfaces(cmd, header, address, buffer, offset);
    }
    cmd.endRendering(dl);
    for (vk::ImageMemoryBarrier2& barrier : barriers) {
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader |
                               vk::PipelineStageFlagBits2::eComputeShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         dl);
}

void RecordLines(vk::CommandBuffer cmd, u64 address) {
    const auto& dl = g_device->dispatch;
    if (!g_lines_cleared) {
        g_lines_cleared = true;
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        const std::array histories{g_lines[0].image, g_lines[1].image, g_occlusion[0].image,
                                   g_occlusion[1].image};
        std::array<vk::ImageMemoryBarrier2, histories.size() + 1> barriers;
        for (u32 i = 0; i < barriers.size(); i++) {
            barriers[i] = {
                .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = i < histories.size() ? histories[i] : g_depth_levels.image,
                .subresourceRange = range,
            };
        }
        barriers.back().subresourceRange.levelCount = DepthLevels;
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                              .pImageMemoryBarriers = barriers.data()},
                             dl);
        for (const vk::Image image : histories) {
            cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                                vk::ClearColorValue{.float32 = std::array<float, 4>{}}, range, dl);
        }
        const vk::MemoryBarrier2 cleared{
            .srcStageMask = vk::PipelineStageFlagBits2::eClear,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .dstAccessMask =
                vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &cleared}, dl);
    }
    g_lines_image = (g_lines_image + 1) % LineImages;
    g_lines_frame = (g_lines_frame + 1) % LineFrames;
    const FramePush push{
        .light = address + Scene::LightOffset,
        .frame = g_lines_frame,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_lines_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_lines_pipeline.layout, 0,
                           g_lines_sets[g_lines_image], {}, dl);
    cmd.pushConstants(g_lines_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                      &push, dl);
    cmd.dispatch((g_extent.width + LinesGroupPixels - 1) / LinesGroupPixels,
                 (g_extent.height + LinesGroupPixels - 1) / LinesGroupPixels, 1, dl);
    WrittenBarrier(cmd);
}

void RecordOcclusion(vk::CommandBuffer cmd, u64 address) {
    const auto& dl = g_device->dispatch;
    const auto groups = [](u32 pixels) {
        return (pixels + LinesGroupPixels - 1) / LinesGroupPixels;
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_depth_level_pipeline.pipeline, dl);
    for (u32 k = 0; k < DepthLevels; k++) {
        const s32 level = static_cast<s32>(k) + 1;
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_depth_level_pipeline.layout, 0,
                               g_depth_level_sets[k], {}, dl);
        cmd.pushConstants(g_depth_level_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0,
                          sizeof(level), &level, dl);
        cmd.dispatch(groups(std::max(g_depth_levels.extent.width >> k, 1u)),
                     groups(std::max(g_depth_levels.extent.height >> k, 1u)), 1, dl);
    }
    const vk::MemoryBarrier2 levels_written{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &levels_written}, dl);
    const FramePush push{
        .light = address + Scene::LightOffset,
        .frame = g_lines_frame,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_occlusion_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_occlusion_pipeline.layout, 0,
                           g_occlusion_sets[g_lines_image], {}, dl);
    cmd.pushConstants(g_occlusion_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0,
                      sizeof(push), &push, dl);
    cmd.dispatch(groups(g_extent.width), groups(g_extent.height), 1, dl);
    WrittenBarrier(cmd);
}

void RecordLight(vk::CommandBuffer cmd, u64 address) {
    const auto& dl = g_device->dispatch;
    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(g_extent.width),
        .height = static_cast<float>(g_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const std::array sets{Textures::Set(), Shadows::Set(), g_sets[g_lines_image]};
    const u64 light = address + Scene::LightOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_light.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_light.layout, 0, sets, {}, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, g_extent}, dl);
    cmd.pushConstants(g_light.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(light),
                      &light, dl);
    cmd.draw(3, 1, 0, 0, dl);
}

} // namespace Gr2Port::World
