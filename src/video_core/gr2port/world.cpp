// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/deferred_frag.h"
#include "video_core/gr2port/shaders/fullscreen_vert.h"
#include "video_core/gr2port/shaders/world_alpha_frag.h"
#include "video_core/gr2port/shaders/world_frag.h"
#include "video_core/gr2port/shaders/world_lines_comp.h"
#include "video_core/gr2port/shaders/world_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/world.h"

namespace Gr2Port::World {

namespace {

// Albedo with the hatching in alpha, the packed normal, and roughness, metallic, translucency and
// emissive.
constexpr std::array<vk::Format, 3> SurfaceFormats{
    vk::Format::eR8G8B8A8Srgb,
    vk::Format::eR16G16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
};

struct SurfacePush {
    u64 draws;
    u64 materials;
};

struct LinesPush {
    u64 light;
    s32 frame;
    u32 padding;
};

// The world's lines: their value, the scaled distance, the frames averaged. Two images take turns
// as the frame's output and the last frame's history.
constexpr vk::Format LinesFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr u32 LineImages = 2;
constexpr s32 LineFrames = 26;
constexpr u32 LinesGroupPixels = 8;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
std::array<Image, SurfaceFormats.size()> g_surfaces;
vk::Extent2D g_extent;
vk::Sampler g_sampler;
vk::Sampler g_linear_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorSetLayout g_lines_layout;
vk::DescriptorPool g_pool;
// Each indexed by the image that holds the frame's lines.
std::array<vk::DescriptorSet, LineImages> g_sets;
std::array<vk::DescriptorSet, LineImages> g_lines_sets;
std::array<Image, LineImages> g_lines;
bool g_lines_cleared = false;
u32 g_lines_image = 0;
s32 g_lines_frame = 0;
Pipeline g_opaque;
Pipeline g_alpha;
Pipeline g_light;
Pipeline g_lines_pipeline;

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
    std::array<vk::DescriptorSetLayoutBinding, 2 + SurfaceFormats.size()> bindings;
    for (u32 i = 0; i < bindings.size(); i++) {
        bindings[i] = {i, vk::DescriptorType::eCombinedImageSampler, 1,
                       vk::ShaderStageFlagBits::eFragment, &g_sampler};
    }
    g_set_layout =
        d.device
            .createDescriptorSetLayout(
                {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
                nullptr, dl)
            .value;
    const std::array<vk::DescriptorSetLayoutBinding, 4> line_bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_linear_sampler},
        {2, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_linear_sampler},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    g_lines_layout =
        d.device
            .createDescriptorSetLayout({.bindingCount = static_cast<u32>(line_bindings.size()),
                                        .pBindings = line_bindings.data()},
                                       nullptr, dl)
            .value;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {vk::DescriptorType::eCombinedImageSampler,
         LineImages * (static_cast<u32>(bindings.size()) + 3)},
        {vk::DescriptorType::eStorageImage, LineImages},
    }};
    g_pool = d.device
                 .createDescriptorPool({.maxSets = 2 * LineImages,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_sampler || !g_linear_sampler || !g_set_layout || !g_lines_layout || !g_pool) {
        return false;
    }
    const std::array set_layouts{g_set_layout, g_set_layout};
    const std::array line_layouts{g_lines_layout, g_lines_layout};
    const auto [result, sets] = d.device.allocateDescriptorSets({.descriptorPool = g_pool,
                                                                 .descriptorSetCount = LineImages,
                                                                 .pSetLayouts = set_layouts.data()},
                                                                dl);
    const auto [line_result, line_sets] =
        d.device.allocateDescriptorSets({.descriptorPool = g_pool,
                                         .descriptorSetCount = LineImages,
                                         .pSetLayouts = line_layouts.data()},
                                        dl);
    if (result != vk::Result::eSuccess || line_result != vk::Result::eSuccess) {
        return false;
    }
    for (u32 i = 0; i < LineImages; i++) {
        g_sets[i] = sets[i];
        g_lines_sets[i] = line_sets[i];
    }
    return true;
}

Pipeline CreateSurfacePipeline(const Device& d, vk::Format depth_format,
                               std::span<const u32> fragment) {
    const std::array texture_set{Textures::SetLayout()};
    return CreateGraphicsPipeline(d, {
                                         .vertex = WORLD_VERT,
                                         .fragment = fragment,
                                         .colors = SurfaceFormats,
                                         .depth = depth_format,
                                         .depth_test = true,
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
    g_opaque = CreateSurfacePipeline(device, depth_format, WORLD_FRAG);
    g_alpha = CreateSurfacePipeline(device, depth_format, WORLD_ALPHA_FRAG);
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
        CreateComputePipeline(device, WORLD_LINES_COMP, sizeof(LinesPush), line_sets);
    return g_opaque.pipeline && g_alpha.pipeline && g_light.pipeline && g_lines_pipeline.pipeline;
}

bool Resize(vk::Extent2D extent, vk::ImageView depth) {
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
    for (Image& image : g_lines) {
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
    g_lines_cleared = false;
    const vk::DescriptorImageInfo depth_info{
        .imageView = depth,
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
    const std::array surface_infos{surface_info(0), surface_info(1), surface_info(2)};
    for (u32 i = 0; i < LineImages; i++) {
        const vk::DescriptorImageInfo output = read_info(g_lines[i], vk::ImageLayout::eGeneral);
        const vk::DescriptorImageInfo history =
            read_info(g_lines[(i + 1) % LineImages], vk::ImageLayout::eGeneral);
        const std::array<vk::WriteDescriptorSet, 9> writes{
            write(g_sets[i], 0, Combined, &depth_info),
            write(g_sets[i], 1, Combined, &surface_infos[0]),
            write(g_sets[i], 2, Combined, &surface_infos[1]),
            write(g_sets[i], 3, Combined, &surface_infos[2]),
            write(g_sets[i], 4, Combined, &output),
            write(g_lines_sets[i], 0, Combined, &depth_info),
            write(g_lines_sets[i], 1, Combined, &surface_infos[1]),
            write(g_lines_sets[i], 2, Combined, &history),
            write(g_lines_sets[i], 3, vk::DescriptorType::eStorageImage, &output),
        };
        g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
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
    SurfacePush push{
        .draws = address + Scene::DrawsOffset,
        .materials = address + Scene::MaterialsOffset,
    };
    if (drawn && header.world_draws != 0) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_opaque.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_opaque.layout, 0,
                               Textures::Set(), {}, dl);
        cmd.pushConstants(g_opaque.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        cmd.drawIndexedIndirect(buffer, offset + Scene::CommandsOffset, header.world_draws,
                                sizeof(Scene::Command), dl);
    }
    if (drawn && header.alpha_draws != 0) {
        const u64 first = Scene::MaxDraws - header.alpha_draws;
        push.draws += first * sizeof(Scene::Draw);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_alpha.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_alpha.layout, 0, Textures::Set(),
                               {}, dl);
        cmd.pushConstants(g_alpha.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        cmd.drawIndexedIndirect(buffer,
                                offset + Scene::CommandsOffset + first * sizeof(Scene::Command),
                                header.alpha_draws, sizeof(Scene::Command), dl);
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
        std::array<vk::ImageMemoryBarrier2, LineImages> barriers;
        for (u32 i = 0; i < LineImages; i++) {
            barriers[i] = {
                .dstStageMask = vk::PipelineStageFlagBits2::eClear,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .image = g_lines[i].image,
                .subresourceRange = range,
            };
        }
        cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                              .pImageMemoryBarriers = barriers.data()},
                             dl);
        for (const Image& image : g_lines) {
            cmd.clearColorImage(image.image, vk::ImageLayout::eGeneral,
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
    const LinesPush push{
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
    // Also orders the next frames' writes after this frame's reads of the images.
    const vk::PipelineStageFlags2 stages =
        vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader;
    const vk::MemoryBarrier2 written{
        .srcStageMask = stages,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = stages,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written}, dl);
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
