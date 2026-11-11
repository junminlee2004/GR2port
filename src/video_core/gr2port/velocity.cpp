// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/shaders/velocity_comp.h"
#include "video_core/gr2port/upscaler.h"
#include "video_core/gr2port/velocity.h"

namespace Gr2Port::Velocity {

namespace {

constexpr vk::Format Format = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::Format MotionFormat = vk::Format::eR16G16Sfloat;
constexpr u32 GroupPixels = 8;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Pipeline g_pipeline;
Image g_image;
// The upscaler's motion, at the scene's size with an upscaler and one pixel without.
Image g_motion;
bool g_upscaler_motion = false;

void ImageBarrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

} // namespace

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    const auto& dl = device.dispatch;
    const vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_sampler = device.device.createSampler(sampler_info, nullptr, dl).value;
    const std::array<vk::DescriptorSetLayoutBinding, 4> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    g_layout = device.device
                   .createDescriptorSetLayout({.bindingCount = static_cast<u32>(bindings.size()),
                                               .pBindings = bindings.data()},
                                              nullptr, dl)
                   .value;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 2},
        {vk::DescriptorType::eStorageImage, 2},
    }};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 1,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_sampler || !g_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_set = sets[0];
    const std::array set_layouts{g_layout};
    g_upscaler_motion = Upscaler::Active();
    const vk::Bool32 upscaler_motion = g_upscaler_motion;
    const vk::SpecializationMapEntry entry{
        .constantID = 0, .offset = 0, .size = sizeof(vk::Bool32)};
    const vk::SpecializationInfo specialization{
        .mapEntryCount = 1,
        .pMapEntries = &entry,
        .dataSize = sizeof(upscaler_motion),
        .pData = &upscaler_motion,
    };
    g_pipeline =
        CreateComputePipeline(device, VELOCITY_COMP, sizeof(u64), set_layouts, &specialization);
    return static_cast<bool>(g_pipeline.pipeline);
}

bool Resize(vk::Extent2D extent, vk::ImageView depth, vk::ImageView character_depth) {
    g_memory->Destroy(g_image);
    g_image =
        g_memory->CreateImage(Format, extent,
                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                  vk::ImageUsageFlagBits::eTransferDst,
                              vk::ImageAspectFlagBits::eColor);
    g_memory->Destroy(g_motion);
    g_motion =
        g_memory->CreateImage(MotionFormat, g_upscaler_motion ? extent : vk::Extent2D{1, 1},
                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                  vk::ImageUsageFlagBits::eTransferDst,
                              vk::ImageAspectFlagBits::eColor);
    if (!g_image.image || !g_motion.image) {
        return false;
    }
    constexpr auto ReadOnly = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    const vk::DescriptorImageInfo depth_info{.imageView = depth, .imageLayout = ReadOnly};
    const vk::DescriptorImageInfo character_info{.imageView = character_depth,
                                                 .imageLayout = ReadOnly};
    const vk::DescriptorImageInfo output_info{.imageView = g_image.view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo motion_info{.imageView = g_motion.view,
                                              .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 4> writes{{
        {
            .dstSet = g_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &depth_info,
        },
        {
            .dstSet = g_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &character_info,
        },
        {
            .dstSet = g_set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &output_info,
        },
        {
            .dstSet = g_set,
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &motion_info,
        },
    }};
    g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
    return true;
}

void Record(vk::CommandBuffer cmd, bool drawn, u64 packet_address) {
    const auto& dl = g_device->dispatch;
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto Clear = vk::PipelineStageFlagBits2::eClear;
    constexpr auto Read = vk::AccessFlagBits2::eShaderSampledRead;
    if (!drawn) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        for (const vk::Image image : {g_image.image, g_motion.image}) {
            ImageBarrier(cmd, image, vk::ImageLayout::eUndefined, Compute,
                         vk::AccessFlagBits2::eNone, Clear, vk::AccessFlagBits2::eTransferWrite);
            cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                                vk::ClearColorValue{.float32 = std::array<float, 4>{}}, range, dl);
            ImageBarrier(cmd, image, vk::ImageLayout::eGeneral, Clear,
                         vk::AccessFlagBits2::eTransferWrite, Compute, Read);
        }
        return;
    }
    for (const vk::Image image : {g_image.image, g_motion.image}) {
        ImageBarrier(cmd, image, vk::ImageLayout::eUndefined, Compute, vk::AccessFlagBits2::eNone,
                     Compute, vk::AccessFlagBits2::eShaderStorageWrite);
    }
    const u64 light = packet_address + Scene::LightOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_pipeline.layout, 0, g_set, {}, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(light),
                      &light, dl);
    cmd.dispatch((g_image.extent.width + GroupPixels - 1) / GroupPixels,
                 (g_image.extent.height + GroupPixels - 1) / GroupPixels, 1, dl);
    for (const vk::Image image : {g_image.image, g_motion.image}) {
        ImageBarrier(cmd, image, vk::ImageLayout::eGeneral, Compute,
                     vk::AccessFlagBits2::eShaderStorageWrite, Compute, Read);
    }
}

vk::ImageView View() {
    return g_image.view;
}

vk::Image MotionImage() {
    return g_motion.image;
}

vk::ImageView MotionView() {
    return g_motion.view;
}

} // namespace Gr2Port::Velocity
