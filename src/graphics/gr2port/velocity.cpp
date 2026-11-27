// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "graphics/gr2port/frame_generation.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/shaders/velocity_comp.h"
#include "graphics/gr2port/upscaler.h"
#include "graphics/gr2port/velocity.h"

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
// The upscaler's and frame generation's motion, at the scene's size with either and one pixel
// without.
Image g_motion;
bool g_upscaler_motion = false;
// The Character list's measured motion, and both halves of the clip history.
bool g_character_motion = false;
Image g_character_motion_image;
Buffer g_clip_history;
// x, y and w of a clip position.
constexpr u64 ClipBytes = 3 * sizeof(float);

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
    const std::array<vk::DescriptorSetLayoutBinding, 5> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {4, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
    }};
    g_layout = CreateSetLayout(device, bindings);
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 3},
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
    g_upscaler_motion = Upscaler::Active() || FrameGeneration::Made();
    g_character_motion = g_upscaler_motion && device.vertex_stores;
    if (g_character_motion) {
        g_clip_history =
            memory.CreateBuffer(2 * u64{Scene::ClipHistoryVertices} * ClipBytes,
                                vk::BufferUsageFlagBits::eStorageBuffer, Placement::Device);
        if (!g_clip_history.buffer) {
            return false;
        }
    }
    const std::array<vk::Bool32, 2> constants{g_upscaler_motion, g_character_motion};
    const std::array<vk::SpecializationMapEntry, 2> entries{{
        {.constantID = 0, .offset = 0, .size = sizeof(vk::Bool32)},
        {.constantID = 1, .offset = sizeof(vk::Bool32), .size = sizeof(vk::Bool32)},
    }};
    const vk::SpecializationInfo specialization{
        .mapEntryCount = static_cast<u32>(entries.size()),
        .pMapEntries = entries.data(),
        .dataSize = sizeof(constants),
        .pData = constants.data(),
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
    g_memory->Destroy(g_character_motion_image);
    g_character_motion_image = g_memory->CreateImage(
        CharacterMotionFormat, g_character_motion ? extent : vk::Extent2D{1, 1},
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
        vk::ImageAspectFlagBits::eColor);
    if (!g_image.image || !g_motion.image || !g_character_motion_image.image) {
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
    const vk::DescriptorImageInfo character_motion_info{
        .imageView = g_character_motion_image.view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const std::array<vk::WriteDescriptorSet, 5> writes{{
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
        {
            .dstSet = g_set,
            .dstBinding = 4,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &character_motion_info,
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
            ImageBarrier(*g_device, cmd, image, vk::ImageLayout::eUndefined,
                         vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eNone, Clear,
                         vk::AccessFlagBits2::eTransferWrite);
            cmd.clearColorImage(image, vk::ImageLayout::eGeneral,
                                vk::ClearColorValue{.float32 = std::array<float, 4>{}}, range, dl);
            ImageBarrier(*g_device, cmd, image, vk::ImageLayout::eGeneral,
                         vk::ImageLayout::eGeneral, Clear, vk::AccessFlagBits2::eTransferWrite,
                         Compute, Read);
        }
        return;
    }
    for (const vk::Image image : {g_image.image, g_motion.image}) {
        ImageBarrier(*g_device, cmd, image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
                     Compute, vk::AccessFlagBits2::eNone, Compute,
                     vk::AccessFlagBits2::eShaderStorageWrite);
    }
    // The program names the measured motion's image either way.
    if (!g_character_motion) {
        ImageBarrier(*g_device, cmd, g_character_motion_image.image, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eShaderReadOnlyOptimal, Compute, vk::AccessFlagBits2::eNone,
                     Compute, Read);
    }
    const u64 light = packet_address + Scene::LightOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_pipeline.layout, 0, g_set, {}, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(light),
                      &light, dl);
    cmd.dispatch((g_image.extent.width + GroupPixels - 1) / GroupPixels,
                 (g_image.extent.height + GroupPixels - 1) / GroupPixels, 1, dl);
    for (const vk::Image image : {g_image.image, g_motion.image}) {
        ImageBarrier(*g_device, cmd, image, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
                     Compute, vk::AccessFlagBits2::eShaderStorageWrite, Compute, Read);
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

bool CharacterMotion() {
    return g_character_motion;
}

vk::Image CharacterMotionImage() {
    return g_character_motion_image.image;
}

vk::ImageView CharacterMotionView() {
    return g_character_motion_image.view;
}

u64 ClipHistory(u64 frame) {
    return g_clip_history.address + (frame & 1) * u64{Scene::ClipHistoryVertices} * ClipBytes;
}

} // namespace Gr2Port::Velocity
