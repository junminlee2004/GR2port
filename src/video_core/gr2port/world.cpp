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

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
std::array<Image, SurfaceFormats.size()> g_surfaces;
vk::Extent2D g_extent;
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Pipeline g_opaque;
Pipeline g_alpha;
Pipeline g_light;

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
    std::array<vk::DescriptorSetLayoutBinding, 1 + SurfaceFormats.size()> bindings;
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
    const vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler,
                                      static_cast<u32>(bindings.size())};
    g_pool = d.device
                 .createDescriptorPool({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size},
                                       nullptr, dl)
                 .value;
    if (!g_sampler || !g_set_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_set_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_set = sets.front();
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
    return g_opaque.pipeline && g_alpha.pipeline && g_light.pipeline;
}

bool Resize(vk::Extent2D extent, vk::ImageView depth) {
    g_extent = extent;
    std::array<vk::DescriptorImageInfo, 1 + SurfaceFormats.size()> infos;
    infos[0] = {.imageView = depth, .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal};
    std::array<vk::WriteDescriptorSet, infos.size()> writes;
    for (u32 i = 0; i < SurfaceFormats.size(); i++) {
        g_memory->Destroy(g_surfaces[i]);
        g_surfaces[i] = g_memory->CreateImage(SurfaceFormats[i], extent,
                                              vk::ImageUsageFlagBits::eColorAttachment |
                                                  vk::ImageUsageFlagBits::eSampled,
                                              vk::ImageAspectFlagBits::eColor);
        if (!g_surfaces[i].image) {
            return false;
        }
        infos[1 + i] = {.imageView = g_surfaces[i].view,
                        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    }
    for (u32 i = 0; i < writes.size(); i++) {
        writes[i] = {
            .dstSet = g_set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &infos[i],
        };
    }
    g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
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
        barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         dl);
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
    const std::array sets{Textures::Set(), Shadows::Set(), g_set};
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
