// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

#include "common/logging/log.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/point_lights.h"
#include "video_core/gr2port/shaders/water_frag.h"
#include "video_core/gr2port/shaders/water_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/upscaler.h"
#include "video_core/gr2port/water.h"

namespace Gr2Port::Water {

namespace {

struct DrawPush {
    u64 draws;
    u64 lights;
    u64 point_lights;
    u64 light_grid;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
vk::Format g_scene_format;
vk::Format g_depth_format;
std::atomic<bool> g_ready{false};
std::thread g_builder;
Pipeline g_pipeline;
Image g_depth_copy;
vk::Sampler g_depth_sampler;
vk::Sampler g_background_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;

// Builds the pipeline, whose programs run to thousands of instructions. The game's state: depth
// tested and written, back faces culled, no blend.
void Build() {
    const auto start = std::chrono::steady_clock::now();
    const std::array sets{Textures::SetLayout(), Shadows::SetLayout(), g_set_layout};
    g_pipeline =
        CreateGraphicsPipeline(*g_device, {
                                              .vertex = WATER_VERT,
                                              .fragment = WATER_FRAG,
                                              .color = g_scene_format,
                                              .depth = g_depth_format,
                                              .cull = vk::CullModeFlagBits::eBack,
                                              .depth_test = true,
                                              .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                              .keep_target_alpha = true,
                                              .push_constant_bytes = sizeof(DrawPush),
                                              .set_layouts = sets,
                                          });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!g_pipeline.pipeline) {
        LOG_ERROR(Render_Vulkan, "GR2port water: the pipeline failed, after {} ms", ms);
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2port water: pipeline built in {} ms", ms);
    SavePipelineCache(*g_device);
    g_ready.store(true, std::memory_order_release);
}

} // namespace

bool Start(const Device& device, Memory& memory, vk::Format scene_format, vk::Format depth_format) {
    g_device = &device;
    g_memory = &memory;
    g_scene_format = scene_format;
    g_depth_format = depth_format;
    const auto& dl = device.dispatch;
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_depth_sampler = device.device.createSampler(sampler_info, nullptr, dl).value;
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    g_background_sampler = device.device.createSampler(sampler_info, nullptr, dl).value;
    constexpr auto Sampled = vk::DescriptorType::eSampledImage;
    constexpr auto Sampler = vk::DescriptorType::eSampler;
    constexpr auto Fragment = vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {0, Sampled, 1, Fragment},
        {1, Sampler, 1, Fragment, &g_depth_sampler},
        {2, Sampled, 1, Fragment},
        {3, Sampler, 1, Fragment, &g_background_sampler},
    }};
    g_set_layout =
        device.device
            .createDescriptorSetLayout(
                {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
                nullptr, dl)
            .value;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {Sampled, 2},
        {Sampler, 2},
    }};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 1,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_depth_sampler || !g_background_sampler || !g_set_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_set_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_set = sets[0];
    g_builder = std::thread{Build};
    g_builder.detach();
    return true;
}

bool Resize(vk::Extent2D extent, vk::ImageView background) {
    g_memory->Destroy(g_depth_copy);
    g_depth_copy = g_memory->CreateImage(g_depth_format, extent,
                                         vk::ImageUsageFlagBits::eTransferDst |
                                             vk::ImageUsageFlagBits::eSampled |
                                             vk::ImageUsageFlagBits::eDepthStencilAttachment,
                                         vk::ImageAspectFlagBits::eDepth);
    if (!g_depth_copy.image) {
        return false;
    }
    const vk::DescriptorImageInfo depth_info{
        .imageView = g_depth_copy.view,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
    };
    const vk::DescriptorImageInfo background_info{
        .imageView = background,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {
            .dstSet = g_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &depth_info,
        },
        {
            .dstSet = g_set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .pImageInfo = &background_info,
        },
    }};
    g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
    return true;
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, vk::ImageView scene, const Image& depth, vk::Extent2D extent) {
    const u32 count = header.effects.water_draws;
    if (count == 0 || !g_ready.load(std::memory_order_acquire)) {
        return;
    }
    const auto& dl = g_device->dispatch;
    constexpr vk::ImageSubresourceRange DepthRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
    constexpr auto Output = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr auto Copy = vk::PipelineStageFlagBits2::eCopy;
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    constexpr vk::AccessFlags2 DepthAccess = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    constexpr auto Attachment = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    constexpr auto Source = vk::ImageLayout::eTransferSrcOptimal;
    constexpr auto Target = vk::ImageLayout::eTransferDstOptimal;
    // The programs read the depth they also write, so they read a copy.
    const std::array<vk::ImageMemoryBarrier2, 2> to_copy{{
        {.srcStageMask = Tests,
         .srcAccessMask = DepthAccess,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
         .oldLayout = Attachment,
         .newLayout = Source,
         .image = depth.image,
         .subresourceRange = DepthRange},
        {.srcStageMask = Fragment,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .oldLayout = vk::ImageLayout::eUndefined,
         .newLayout = Target,
         .image = g_depth_copy.image,
         .subresourceRange = DepthRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(to_copy.size()),
                          .pImageMemoryBarriers = to_copy.data()},
                         dl);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
        .extent = vk::Extent3D{extent.width, extent.height, 1},
    };
    cmd.copyImage(depth.image, Source, g_depth_copy.image, Target, region, dl);
    const std::array<vk::ImageMemoryBarrier2, 2> copied{{
        {.srcStageMask = Copy,
         .dstStageMask = Tests,
         .dstAccessMask = DepthAccess,
         .oldLayout = Source,
         .newLayout = Attachment,
         .image = depth.image,
         .subresourceRange = DepthRange},
        {.srcStageMask = Copy,
         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .dstStageMask = Fragment,
         .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
         .oldLayout = Target,
         .newLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
         .image = g_depth_copy.image,
         .subresourceRange = DepthRange},
    }};
    const vk::MemoryBarrier2 colors{
        .srcStageMask = Output,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = Output,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1,
                          .pMemoryBarriers = &colors,
                          .imageMemoryBarrierCount = static_cast<u32>(copied.size()),
                          .pImageMemoryBarriers = copied.data()},
                         dl);
    const vk::RenderingAttachmentInfo color{
        .imageView = scene,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth.view,
        .imageLayout = Attachment,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color,
                        .pDepthAttachment = &depth_attachment},
                       dl);
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport = Upscaler::SceneViewport(extent);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    const std::array sets{Textures::Set(), Shadows::Set(), g_set};
    const DrawPush push{
        .draws = address + Scene::WaterDrawsOffset,
        .lights = address + Scene::ParticleLightOffset,
        .point_lights = address + Scene::PointLightsOffset,
        .light_grid = PointLights::GridAddress(),
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_pipeline.layout, 0, sets, {}, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                      &push, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    cmd.drawIndexedIndirect(buffer, offset + Scene::WaterCommandsOffset, count,
                            sizeof(Scene::Command), dl);
    cmd.endRendering(dl);
}

} // namespace Gr2Port::Water
