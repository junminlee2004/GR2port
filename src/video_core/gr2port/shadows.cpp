// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/particles.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/shadow_alpha_frag.h"
#include "video_core/gr2port/shaders/shadow_alpha_vert.h"
#include "video_core/gr2port/shaders/shadow_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/vegetation.h"

namespace Gr2Port::Shadows {

namespace {

// A 2048x2048 map for each of the first two cascades and a 4096x4096 map for the last.
constexpr u32 LastCascade = Scene::Cascades - 1;
constexpr u32 ArraySize = 2048;
constexpr u32 LastSize = 4096;
// GCN polygon offset slopes count sixteenths.
constexpr float SlopeScale = 1.0f / 16.0f;

struct PushConstants {
    u64 casters;
    u64 shadows;
    u32 cascade;
    u32 padding;
};

const Device* g_device = nullptr;
// The first two cascades' layers, and the last cascade's map.
Image g_maps;
Image g_last_map;
std::array<vk::ImageView, Scene::Cascades> g_layers;
vk::Sampler g_sampler;
vk::Sampler g_depth_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
// Indexed by caster list.
std::array<Pipeline, Scene::CasterLists> g_pipelines;

bool AlphaTested(u32 list) {
    return list >= 2;
}

u32 MapSize(u32 cascade) {
    return cascade == LastCascade ? LastSize : ArraySize;
}

bool CullsBackFaces(u32 list) {
    return list % 2 == 0;
}

bool CreateSet(const Device& d) {
    const auto& dl = d.dispatch;
    const auto features = d.gpu.getFormatProperties(DepthFormat, dl).optimalTilingFeatures;
    const vk::Filter filter = (features & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)
                                  ? vk::Filter::eLinear
                                  : vk::Filter::eNearest;
    const vk::SamplerCreateInfo sampler_info{
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .compareEnable = true,
        .compareOp = vk::CompareOp::eLess,
    };
    g_sampler = d.device.createSampler(sampler_info, nullptr, dl).value;
    const vk::SamplerCreateInfo depth_sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_depth_sampler = d.device.createSampler(depth_sampler_info, nullptr, dl).value;
    constexpr vk::ShaderStageFlags Stages =
        vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute;
    constexpr auto Combined = vk::DescriptorType::eCombinedImageSampler;
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {0, Combined, 1, Stages, &g_sampler},
        {1, Combined, 1, Stages, &g_depth_sampler},
        {2, Combined, 1, Stages, &g_sampler},
        {3, Combined, 1, Stages, &g_depth_sampler},
    }};
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
    if (!g_sampler || !g_depth_sampler || !g_set_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_set_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_set = sets.front();
    const vk::DescriptorImageInfo array_info{
        .imageView = g_maps.view,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo last_info{
        .imageView = g_last_map.view,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
    };
    std::array<vk::WriteDescriptorSet, 4> writes;
    for (u32 b = 0; b < writes.size(); b++) {
        writes[b] = {
            .dstSet = g_set,
            .dstBinding = b,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = b < 2 ? &array_info : &last_info,
        };
    }
    d.device.updateDescriptorSets(writes, {}, dl);
    return true;
}

void Transition(vk::CommandBuffer cmd, vk::ImageLayout from, vk::ImageLayout to,
                vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 array_barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = g_maps.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, LastCascade},
    };
    std::array barriers{array_barrier, array_barrier};
    barriers[1].image = g_last_map.image;
    barriers[1].subresourceRange.layerCount = 1;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         g_device->dispatch);
}

} // namespace

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    const auto& dl = device.dispatch;
    const vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
    g_maps = memory.CreateImage(DepthFormat, vk::Extent2D{ArraySize, ArraySize}, usage,
                                vk::ImageAspectFlagBits::eDepth, LastCascade);
    g_last_map = memory.CreateImage(DepthFormat, vk::Extent2D{LastSize, LastSize}, usage,
                                    vk::ImageAspectFlagBits::eDepth);
    if (!g_maps.image || !g_last_map.image) {
        return false;
    }
    g_layers[LastCascade] = g_last_map.view;
    for (u32 c = 0; c < LastCascade; c++) {
        const vk::ImageViewCreateInfo info{
            .image = g_maps.image,
            .viewType = vk::ImageViewType::e2D,
            .format = DepthFormat,
            .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, c, 1},
        };
        g_layers[c] = device.device.createImageView(info, nullptr, dl).value;
    }
    if (!CreateSet(device)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the shadow cascades' set failed");
        return false;
    }
    const std::array texture_set{Textures::SetLayout()};
    for (u32 list = 0; list < Scene::CasterLists; list++) {
        const bool alpha_tested = AlphaTested(list);
        g_pipelines[list] = CreateGraphicsPipeline(
            device,
            {
                .vertex = alpha_tested ? std::span<const u32>{SHADOW_ALPHA_VERT}
                                       : std::span<const u32>{SHADOW_VERT},
                .fragment =
                    alpha_tested ? std::span<const u32>{SHADOW_ALPHA_FRAG} : std::span<const u32>{},
                .depth = DepthFormat,
                .cull = CullsBackFaces(list) ? vk::CullModeFlagBits::eBack
                                             : vk::CullModeFlagBits::eNone,
                .depth_test = true,
                .depth_compare = vk::CompareOp::eLessOrEqual,
                .depth_bias = true,
                .push_constant_bytes = sizeof(PushConstants),
                .set_layouts = alpha_tested ? std::span<const vk::DescriptorSetLayout>{texture_set}
                                            : std::span<const vk::DescriptorSetLayout>{},
            });
        if (!g_pipelines[list].pipeline) {
            return false;
        }
    }
    return true;
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
            vk::Buffer buffer, u64 offset, vk::QueryPool timestamps, u32 first_query) {
    const auto& dl = g_device->dispatch;
    Transition(cmd, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal,
               vk::PipelineStageFlagBits2::eFragmentShader |
                   vk::PipelineStageFlagBits2::eComputeShader,
               vk::AccessFlagBits2::eNone,
               vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                   vk::PipelineStageFlagBits2::eLateFragmentTests,
               vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                   vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
    PushConstants push{
        .casters = address + Scene::CastersOffset,
        .shadows = address + Scene::ShadowsOffset,
    };
    for (u32 c = 0; c < Scene::Cascades; c++) {
        const u32 size = MapSize(c);
        // Flipped as the main view's, which keeps the game's front faces.
        const vk::Viewport viewport{
            .x = 0.0f,
            .y = static_cast<float>(size),
            .width = static_cast<float>(size),
            .height = -static_cast<float>(size),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        const vk::Rect2D scissor{{0, 0}, vk::Extent2D{size, size}};
        const vk::RenderingAttachmentInfo depth{
            .imageView = g_layers[c],
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = {.depthStencil = {.depth = 1.0f}},
        };
        cmd.beginRendering({.renderArea = scissor, .layerCount = 1, .pDepthAttachment = &depth},
                           dl);
        cmd.setViewport(0, viewport, dl);
        cmd.setScissor(0, scissor, dl);
        cmd.setDepthBias(header.depth_bias[c][0], 0.0f, header.depth_bias[c][1] * SlopeScale, dl);
        push.cascade = c;
        for (u32 list = 0; list < Scene::CasterLists; list++) {
            const u32 count = header.caster_commands[c][list];
            if ((header.cascades >> c & 1) == 0 || count == 0) {
                continue;
            }
            const Pipeline& pipeline = g_pipelines[list];
            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
            if (AlphaTested(list)) {
                cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0,
                                       Textures::Set(), {}, dl);
            }
            cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                              sizeof(push), &push, dl);
            cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
            const u64 first = (u64{c} * Scene::CasterLists + list) * Scene::MaxCasters;
            cmd.drawIndexedIndirect(
                buffer, offset + Scene::CasterCommandsOffset + first * sizeof(Scene::Command),
                count, sizeof(Scene::Command), dl);
        }
        Vegetation::RecordShadows(cmd, header, address, buffer, offset, c);
        if ((header.cascades >> c & 1) != 0) {
            Particles::RecordShadows(cmd, header, packet, address, buffer, offset, c);
        }
        cmd.endRendering(dl);
        if (timestamps) {
            cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, timestamps,
                                first_query + c, dl);
        }
    }
    Transition(cmd, vk::ImageLayout::eDepthStencilAttachmentOptimal,
               vk::ImageLayout::eDepthStencilReadOnlyOptimal,
               vk::PipelineStageFlagBits2::eLateFragmentTests,
               vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
               vk::PipelineStageFlagBits2::eFragmentShader |
                   vk::PipelineStageFlagBits2::eComputeShader,
               vk::AccessFlagBits2::eShaderSampledRead);
}

vk::DescriptorSetLayout SetLayout() {
    return g_set_layout;
}

vk::DescriptorSet Set() {
    return g_set;
}

} // namespace Gr2Port::Shadows
