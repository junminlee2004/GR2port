// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/particles.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/self_translucent.h"
#include "video_core/gr2port/shaders/self_translucent_frag.h"
#include "video_core/gr2port/shaders/self_translucent_vert.h"

namespace Gr2Port::SelfTranslucent {

namespace {

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
Pipeline g_pipeline;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Image g_color;
Image g_depth;
vk::ImageView g_depth_read;
vk::Extent2D g_extent;
// Whether the targets are clear and readable, as they stay while no character needs them.
bool g_clear = false;

void WriteImage(u32 binding, vk::ImageView view, vk::ImageLayout layout) {
    const vk::DescriptorImageInfo info{.imageView = view, .imageLayout = layout};
    const vk::WriteDescriptorSet write{
        .dstSet = g_set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
}

void Barrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageAspectFlags aspect,
             vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
             vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
             vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

} // namespace

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    const auto& dl = device.dispatch;
    constexpr auto Sampled = vk::DescriptorType::eSampledImage;
    constexpr auto Fragment = vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {0, Sampled, 1, Fragment},
        {1, Sampled, 1, Fragment},
    }};
    g_set_layout =
        device.device
            .createDescriptorSetLayout(
                {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
                nullptr, dl)
            .value;
    const vk::DescriptorPoolSize size{Sampled, 2};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size},
                                       nullptr, dl)
                 .value;
    if (!g_set_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_set_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_set = sets[0];
    // The game's state: front faces culled, so depth holds the backs of the skin.
    g_pipeline = CreateGraphicsPipeline(device, {
                                                    .vertex = SELF_TRANSLUCENT_VERT,
                                                    .fragment = SELF_TRANSLUCENT_FRAG,
                                                    .color = ColorFormat,
                                                    .depth = DepthFormat,
                                                    .cull = vk::CullModeFlagBits::eFront,
                                                    .depth_test = true,
                                                    .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                                    .stencil_replace = true,
                                                    .push_constant_bytes = sizeof(u64),
                                                });
    return static_cast<bool>(g_pipeline.pipeline);
}

bool Resize(vk::Extent2D extent) {
    const auto& dl = g_device->dispatch;
    if (g_depth_read) {
        g_device->device.destroyImageView(g_depth_read, nullptr, dl);
        g_depth_read = nullptr;
    }
    g_memory->Destroy(g_color);
    g_memory->Destroy(g_depth);
    g_color = g_memory->CreateImage(ColorFormat, extent,
                                    vk::ImageUsageFlagBits::eColorAttachment |
                                        vk::ImageUsageFlagBits::eSampled,
                                    vk::ImageAspectFlagBits::eColor);
    g_depth = g_memory->CreateImage(
        DepthFormat, extent,
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
        vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil);
    if (!g_color.image || !g_depth.image) {
        return false;
    }
    const auto [result, view] = g_device->device.createImageView(
        {.image = g_depth.image,
         .viewType = vk::ImageViewType::e2D,
         .format = DepthFormat,
         .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1}},
        nullptr, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_depth_read = view;
    WriteImage(0, g_color.view, vk::ImageLayout::eShaderReadOnlyOptimal);
    WriteImage(1, g_depth_read, vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    g_extent = extent;
    g_clear = false;
    return true;
}

vk::DescriptorSetLayout SetLayout() {
    return g_set_layout;
}

vk::DescriptorSet Set() {
    return g_set;
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
            vk::Buffer buffer, u64 offset) {
    const auto& dl = g_device->dispatch;
    const u32 first_list = static_cast<u32>(Scene::DrawList::SelfTranslucent1);
    u32 bodies = 0;
    for (u32 i = 0; i < Scene::SelfTranslucentLists; i++) {
        bodies += header.draw_commands[first_list + i];
    }
    if (bodies == 0 && g_clear) {
        return;
    }
    g_clear = bodies == 0;
    const vk::ImageAspectFlags depth_aspect =
        vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    constexpr auto Tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    constexpr auto DepthWrite = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    constexpr auto DepthAccess = vk::AccessFlagBits2::eDepthStencilAttachmentRead | DepthWrite;
    constexpr auto ColorStage = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr auto ColorWrite = vk::AccessFlagBits2::eColorAttachmentWrite;
    constexpr auto ColorAccess = vk::AccessFlagBits2::eColorAttachmentRead | ColorWrite;
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Sampled = vk::AccessFlagBits2::eShaderSampledRead;
    // Last frame's contents do not matter: the targets are cleared.
    Barrier(cmd, g_color.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal, Fragment, {}, ColorStage, ColorAccess);
    Barrier(cmd, g_depth.image, depth_aspect, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eDepthStencilAttachmentOptimal, Fragment, {}, Tests, DepthAccess);
    const vk::RenderingAttachmentInfo color{
        .imageView = g_color.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = {.color = vk::ClearColorValue{.float32 = std::array<float, 4>{}}},
    };
    // Reverse depth: 0 is far.
    const vk::RenderingAttachmentInfo depth{
        .imageView = g_depth.view,
        .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{0.0f, 0}},
    };
    cmd.beginRendering({.renderArea = {{0, 0}, g_extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color,
                        .pDepthAttachment = &depth,
                        .pStencilAttachment = &depth},
                       dl);
    if (bodies != 0) {
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
        const u64 draws = address + Scene::DrawsOffset;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_pipeline.pipeline, dl);
        cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0,
                          sizeof(draws), &draws, dl);
        cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
        for (u32 i = 0; i < Scene::SelfTranslucentLists; i++) {
            const auto list = static_cast<Scene::DrawList>(first_list + i);
            const u32 count = header.draw_commands[first_list + i];
            if (count == 0) {
                continue;
            }
            cmd.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack, i + 1, dl);
            cmd.drawIndexedIndirect(buffer,
                                    offset + Scene::CommandsOffset +
                                        u64{Scene::FirstCommand(header, list)} *
                                            sizeof(Scene::Command),
                                    count, sizeof(Scene::Command), dl);
        }
        Particles::RecordSelfTranslucent(cmd, header, packet, address, buffer, offset);
    }
    cmd.endRendering(dl);
    Barrier(cmd, g_color.image, vk::ImageAspectFlagBits::eColor,
            vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            ColorStage, ColorWrite, Fragment, Sampled);
    Barrier(cmd, g_depth.image, depth_aspect, vk::ImageLayout::eDepthStencilAttachmentOptimal,
            vk::ImageLayout::eDepthStencilReadOnlyOptimal, Tests, DepthWrite, Fragment, Sampled);
}

} // namespace Gr2Port::SelfTranslucent
