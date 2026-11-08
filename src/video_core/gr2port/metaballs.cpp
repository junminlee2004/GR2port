// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>

#include "common/logging/log.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/metaballs.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/fullscreen_vert.h"
#include "video_core/gr2port/shaders/metaball_frag.h"
#include "video_core/gr2port/shaders/metaball_skinned_vert.h"
#include "video_core/gr2port/shaders/metaball_vert.h"
#include "video_core/gr2port/shaders/surface_blur_frag.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/world.h"

namespace Gr2Port::Metaballs {

namespace {

constexpr vk::Format MaskFormat = vk::Format::eR8Unorm;
// The normal surface, which the blur reads and writes.
constexpr vk::Format NormalFormat = World::SurfaceFormats[1];
// The blur's axes as the game's SurfaceBlur sets them, across then down: an int2 and padding each.
constexpr std::array<u32, 8> Axes{1, 0, 0, 0, 0, 1, 0, 0};
constexpr u64 AxisBytes = 16;
constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
// The unskinned and the skinned draws.
std::array<Pipeline, 2> g_pipelines;
Pipeline g_blur;
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_blur_layout;
vk::DescriptorPool g_pool;
// The blur's sets: from the normals, and from the temporary image.
std::array<vk::DescriptorSet, 2> g_blur_sets;
Buffer g_axes;
Image g_mask;
Image g_temporary;
vk::Extent2D g_extent;

void ImageBarrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = ColorRange,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

// The writes of one rendering into the surfaces and depth, before the next loads them.
void AttachmentBarrier(vk::CommandBuffer cmd) {
    constexpr auto Stages = vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                            vk::PipelineStageFlagBits2::eLateFragmentTests;
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = Stages,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
        .dstStageMask = Stages,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite |
                         vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier},
                         g_device->dispatch);
}

// Inside the target's own rendering, where the mask is set: one pass of the game's blur.
void BlurPass(vk::CommandBuffer cmd, vk::ImageView target, vk::DescriptorSet set, u64 axis) {
    const auto& dl = g_device->dispatch;
    const vk::RenderingAttachmentInfo color{
        .imageView = target,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, g_extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color},
                       dl);
    const vk::Viewport viewport{
        .width = static_cast<float>(g_extent.width),
        .height = static_cast<float>(g_extent.height),
        .maxDepth = 1.0f,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_blur.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_blur.layout, 0, set, {}, dl);
    cmd.pushConstants(g_blur.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(axis), &axis,
                      dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, g_extent}, dl);
    cmd.draw(3, 1, 0, 0, dl);
    cmd.endRendering(dl);
}

} // namespace

bool Start(const Device& device, Memory& memory, vk::Format depth_format) {
    g_device = &device;
    g_memory = &memory;
    const auto& dl = device.dispatch;
    const std::array<vk::Format, 4> colors{World::SurfaceFormats[0], World::SurfaceFormats[1],
                                           World::SurfaceFormats[2], MaskFormat};
    const std::array draw_sets{Textures::SetLayout()};
    const std::array<std::span<const u32>, 2> vertex{METABALL_VERT, METABALL_SKINNED_VERT};
    for (u32 skinned = 0; skinned < g_pipelines.size(); skinned++) {
        g_pipelines[skinned] =
            CreateGraphicsPipeline(device, {
                                               .vertex = vertex[skinned],
                                               .fragment = METABALL_FRAG,
                                               .colors = colors,
                                               .depth = depth_format,
                                               .depth_test = true,
                                               .push_constant_bytes = sizeof(u64),
                                               .set_layouts = draw_sets,
                                           });
        if (!g_pipelines[skinned].pipeline) {
            return false;
        }
    }
    g_sampler = device.device
                    .createSampler({.magFilter = vk::Filter::eNearest,
                                    .minFilter = vk::Filter::eNearest,
                                    .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                    .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                    .addressModeW = vk::SamplerAddressMode::eClampToEdge},
                                   nullptr, dl)
                    .value;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment},
        {1, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eFragment, &g_sampler},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eFragment},
    }};
    g_blur_layout =
        device.device
            .createDescriptorSetLayout(
                {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
                nullptr, dl)
            .value;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {vk::DescriptorType::eSampledImage, 4},
        {vk::DescriptorType::eSampler, 2},
    }};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 2,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_sampler || !g_blur_layout || !g_pool) {
        return false;
    }
    const std::array layouts{g_blur_layout, g_blur_layout};
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool,
         .descriptorSetCount = static_cast<u32>(layouts.size()),
         .pSetLayouts = layouts.data()},
        dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_blur_sets = {sets[0], sets[1]};
    const std::array blur_sets{g_blur_layout};
    g_blur = CreateGraphicsPipeline(device, {
                                                .vertex = FULLSCREEN_VERT,
                                                .fragment = SURFACE_BLUR_FRAG,
                                                .color = NormalFormat,
                                                .push_constant_bytes = sizeof(u64),
                                                .set_layouts = blur_sets,
                                            });
    g_axes = memory.CreateBuffer(sizeof(Axes), vk::BufferUsageFlagBits::eStorageBuffer,
                                 Placement::Upload);
    if (!g_blur.pipeline || g_axes.mapped == nullptr) {
        return false;
    }
    std::memcpy(g_axes.mapped, Axes.data(), sizeof(Axes));
    return true;
}

bool Resize(vk::Extent2D extent, vk::ImageView normal) {
    g_extent = extent;
    g_memory->Destroy(g_mask);
    g_memory->Destroy(g_temporary);
    g_mask = g_memory->CreateImage(MaskFormat, extent,
                                   vk::ImageUsageFlagBits::eColorAttachment |
                                       vk::ImageUsageFlagBits::eSampled,
                                   vk::ImageAspectFlagBits::eColor);
    g_temporary = g_memory->CreateImage(NormalFormat, extent,
                                        vk::ImageUsageFlagBits::eColorAttachment |
                                            vk::ImageUsageFlagBits::eSampled |
                                            vk::ImageUsageFlagBits::eTransferDst,
                                        vk::ImageAspectFlagBits::eColor);
    if (!g_mask.image || !g_temporary.image) {
        return false;
    }
    const std::array<vk::ImageView, 2> sources{normal, g_temporary.view};
    for (u32 i = 0; i < g_blur_sets.size(); i++) {
        const vk::DescriptorImageInfo source{
            .imageView = sources[i],
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const vk::DescriptorImageInfo mask{
            .imageView = g_mask.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        };
        const std::array<vk::WriteDescriptorSet, 2> writes{{
            {.dstSet = g_blur_sets[i],
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &source},
            {.dstSet = g_blur_sets[i],
             .dstBinding = 2,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eSampledImage,
             .pImageInfo = &mask},
        }};
        g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
    }
    return true;
}

void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                    vk::Buffer buffer, u64 offset,
                    std::span<const vk::RenderingAttachmentInfo> colors,
                    const vk::RenderingAttachmentInfo& depth) {
    const auto& dl = g_device->dispatch;
    if (header.metaballs == 0 || colors.size() != World::SurfaceFormats.size()) {
        return;
    }
    AttachmentBarrier(cmd);
    // The last frame's blur read the mask.
    ImageBarrier(cmd, g_mask.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eColorAttachmentOptimal,
                 vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                 vk::AccessFlagBits2::eColorAttachmentWrite);
    std::array<vk::RenderingAttachmentInfo, 4> attachments{colors[0], colors[1], colors[2]};
    attachments[3] = {
        .imageView = g_mask.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = {.color = vk::ClearColorValue{.float32 = std::array<float, 4>{}}},
    };
    cmd.beginRendering({.renderArea = {{0, 0}, g_extent},
                        .layerCount = 1,
                        .colorAttachmentCount = static_cast<u32>(attachments.size()),
                        .pColorAttachments = attachments.data(),
                        .pDepthAttachment = &depth},
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
    const u64 draws = address + Scene::MetaballDrawsOffset;
    for (u32 list = 0; list < g_pipelines.size(); list++) {
        const u32 count = header.metaball_commands[list];
        if (count == 0) {
            continue;
        }
        const Pipeline& pipeline = g_pipelines[list];
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0,
                               Textures::Set(), {}, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(draws),
                          &draws, dl);
        cmd.drawIndexedIndirect(buffer,
                                offset + Scene::MetaballCommandsOffset +
                                    u64{list} * Scene::MaxMetaballs * sizeof(Scene::Command),
                                count, sizeof(Scene::Command), dl);
    }
    cmd.endRendering(dl);
    AttachmentBarrier(cmd);
    ImageBarrier(
        cmd, g_mask.image, vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead);
}

void RecordBlur(vk::CommandBuffer cmd, const Scene::Header& header, const Image& normal) {
    const auto& dl = g_device->dispatch;
    if (header.metaballs == 0) {
        return;
    }
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Read = vk::AccessFlagBits2::eShaderSampledRead;
    constexpr auto Attachment = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr auto Written = vk::AccessFlagBits2::eColorAttachmentWrite;
    // The game first sets the blur's image to 0 outside the metaballs.
    ImageBarrier(cmd, g_temporary.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eTransferDstOptimal, Fragment, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite);
    cmd.clearColorImage(g_temporary.image, vk::ImageLayout::eTransferDstOptimal,
                        vk::ClearColorValue{.float32 = std::array<float, 4>{}}, ColorRange, dl);
    ImageBarrier(cmd, g_temporary.image, vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eClear,
                 vk::AccessFlagBits2::eTransferWrite, Attachment,
                 vk::AccessFlagBits2::eColorAttachmentRead | Written);
    BlurPass(cmd, g_temporary.view, g_blur_sets[0], g_axes.address);
    ImageBarrier(cmd, g_temporary.image, vk::ImageLayout::eColorAttachmentOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Attachment, Written, Fragment, Read);
    ImageBarrier(cmd, normal.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eColorAttachmentOptimal, Fragment, Read, Attachment,
                 vk::AccessFlagBits2::eColorAttachmentRead | Written);
    BlurPass(cmd, normal.view, g_blur_sets[1], g_axes.address + AxisBytes);
    ImageBarrier(cmd, normal.image, vk::ImageLayout::eColorAttachmentOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Attachment, Written,
                 Fragment | vk::PipelineStageFlagBits2::eComputeShader, Read);
}

} // namespace Gr2Port::Metaballs
