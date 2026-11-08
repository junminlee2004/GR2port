// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <span>
#include <vector>

#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/motion_blur.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/shaders/motion_blur_compress_comp.h"
#include "video_core/gr2port/shaders/motion_blur_frag.h"
#include "video_core/gr2port/shaders/motion_blur_neighbor_comp.h"
#include "video_core/gr2port/shaders/motion_blur_tile_comp.h"
#include "video_core/gr2port/shaders/motion_blur_vert.h"

namespace Gr2Port::MotionBlur {

namespace {

constexpr vk::Format WorkFormat = vk::Format::eR16G16B16A16Sfloat;
// The game's tiles, and the compression's group of 16 by 16 pixels.
constexpr vk::Extent2D Tiles{64, 60};
constexpr u32 GroupPixels = 16;
constexpr u64 TileConstantsOffset = 32;
constexpr u64 BlurConstantsOffset = 64;

constexpr auto Sampled = vk::DescriptorType::eSampledImage;
constexpr auto Combined = vk::DescriptorType::eCombinedImageSampler;
constexpr auto Storage = vk::DescriptorType::eStorageImage;
constexpr auto SamplerType = vk::DescriptorType::eSampler;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
vk::Format g_scene_format;
vk::Sampler g_point;
vk::Sampler g_linear;
vk::DescriptorSetLayout g_compress_layout;
vk::DescriptorSetLayout g_max_layout;
vk::DescriptorSetLayout g_blur_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_compress_set;
vk::DescriptorSet g_tile_set;
vk::DescriptorSet g_neighbor_set;
// The reconstruction from the scene into the second image, and back.
std::array<vk::DescriptorSet, 2> g_blur_sets;
Pipeline g_compress;
Pipeline g_tile;
Pipeline g_neighbor;
Pipeline g_blur;
Image g_compressed;
Image g_tile_max;
Image g_neighbor_max;
Image g_second;
vk::Image g_scene;
vk::ImageView g_scene_view;

vk::DescriptorSetLayout CreateLayout(std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    return g_device->device
        .createDescriptorSetLayout(
            {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
            nullptr, g_device->dispatch)
        .value;
}

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
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

// Draws the reconstruction from the image of set into target.
void Reconstruct(vk::CommandBuffer cmd, vk::DescriptorSet set, vk::ImageView target, u64 address) {
    const auto& dl = g_device->dispatch;
    const vk::RenderingAttachmentInfo color{
        .imageView = target,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::Extent2D extent = g_second.extent;
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color},
                       dl);
    cmd.setViewport(0,
                    vk::Viewport{.width = static_cast<float>(extent.width),
                                 .height = static_cast<float>(extent.height),
                                 .maxDepth = 1.0f},
                    dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    const u64 constants = address + Scene::MotionBlurOffset + BlurConstantsOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_blur.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_blur.layout, 0, set, {}, dl);
    cmd.pushConstants(g_blur.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(constants),
                      &constants, dl);
    cmd.draw(3, 1, 0, 0, dl);
    cmd.endRendering(dl);
}

} // namespace

bool Start(const Device& device, Memory& memory, vk::Format scene_format) {
    g_device = &device;
    g_memory = &memory;
    g_scene_format = scene_format;
    const auto& dl = device.dispatch;
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_point = device.device.createSampler(sampler_info, nullptr, dl).value;
    sampler_info.magFilter = vk::Filter::eLinear;
    sampler_info.minFilter = vk::Filter::eLinear;
    g_linear = device.device.createSampler(sampler_info, nullptr, dl).value;
    constexpr auto Compute = vk::ShaderStageFlagBits::eCompute;
    constexpr auto Fragment = vk::ShaderStageFlagBits::eFragment;
    const std::array<vk::DescriptorSetLayoutBinding, 4> compress_bindings{{
        {0, Sampled, 1, Compute},
        {1, Sampled, 1, Compute},
        {2, SamplerType, 1, Compute, &g_point},
        {3, Storage, 1, Compute},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 2> max_bindings{{
        {0, Combined, 1, Compute, &g_point},
        {1, Storage, 1, Compute},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 7> blur_bindings{{
        {0, Sampled, 1, Fragment},
        {1, Sampled, 1, Fragment},
        {2, Sampled, 1, Fragment},
        {3, SamplerType, 1, Fragment, &g_linear},
        {4, SamplerType, 1, Fragment, &g_point},
        {5, Sampled, 1, Fragment},
        {6, Sampled, 1, Fragment},
    }};
    g_compress_layout = CreateLayout(compress_bindings);
    g_max_layout = CreateLayout(max_bindings);
    g_blur_layout = CreateLayout(blur_bindings);
    const std::array<vk::DescriptorPoolSize, 4> sizes{{
        {Sampled, 2 + 2 * 5},
        {SamplerType, 1 + 2 * 2},
        {Combined, 2},
        {Storage, 3},
    }};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 5,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_point || !g_linear || !g_compress_layout || !g_max_layout || !g_blur_layout || !g_pool) {
        return false;
    }
    const std::array layouts{g_compress_layout, g_max_layout, g_max_layout, g_blur_layout,
                             g_blur_layout};
    const auto [result, sets] = device.device.allocateDescriptorSets(
        {.descriptorPool = g_pool,
         .descriptorSetCount = static_cast<u32>(layouts.size()),
         .pSetLayouts = layouts.data()},
        dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_compress_set = sets[0];
    g_tile_set = sets[1];
    g_neighbor_set = sets[2];
    g_blur_sets = {sets[3], sets[4]};
    const std::array compress_sets{g_compress_layout};
    const std::array max_sets{g_max_layout};
    const std::array blur_sets{g_blur_layout};
    g_compress =
        CreateComputePipeline(device, MOTION_BLUR_COMPRESS_COMP, sizeof(u64), compress_sets);
    g_tile = CreateComputePipeline(device, MOTION_BLUR_TILE_COMP, sizeof(u64), max_sets);
    g_neighbor = CreateComputePipeline(device, MOTION_BLUR_NEIGHBOR_COMP, 0, max_sets);
    g_blur = CreateGraphicsPipeline(device, {
                                                .vertex = MOTION_BLUR_VERT,
                                                .fragment = MOTION_BLUR_FRAG,
                                                .color = scene_format,
                                                .push_constant_bytes = sizeof(u64),
                                                .set_layouts = blur_sets,
                                            });
    return g_compress.pipeline && g_tile.pipeline && g_neighbor.pipeline && g_blur.pipeline;
}

bool Resize(vk::Extent2D extent, vk::ImageView velocity, vk::ImageView depth,
            vk::ImageView character_depth, vk::Image scene, vk::ImageView scene_view) {
    for (Image* image : {&g_compressed, &g_tile_max, &g_neighbor_max, &g_second}) {
        g_memory->Destroy(*image);
    }
    constexpr auto Work = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    constexpr auto Color = vk::ImageAspectFlagBits::eColor;
    g_compressed = g_memory->CreateImage(WorkFormat, extent, Work, Color);
    g_tile_max = g_memory->CreateImage(WorkFormat, Tiles, Work, Color);
    g_neighbor_max = g_memory->CreateImage(WorkFormat, Tiles, Work, Color);
    g_second = g_memory->CreateImage(
        g_scene_format, extent,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, Color);
    if (!g_compressed.image || !g_tile_max.image || !g_neighbor_max.image || !g_second.image) {
        return false;
    }
    g_scene = scene;
    g_scene_view = scene_view;
    constexpr auto General = vk::ImageLayout::eGeneral;
    constexpr auto DepthRead = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    constexpr auto ShaderRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    const auto info = [](vk::ImageView view, vk::ImageLayout layout) {
        return vk::DescriptorImageInfo{.imageView = view, .imageLayout = layout};
    };
    const std::array infos{
        info(velocity, General),            // 0
        info(depth, DepthRead),             // 1
        info(g_compressed.view, General),   // 2
        info(g_tile_max.view, General),     // 3
        info(g_neighbor_max.view, General), // 4
        info(scene_view, ShaderRead),       // 5
        info(g_second.view, ShaderRead),    // 6
        info(character_depth, DepthRead),   // 7
    };
    const auto write = [](vk::DescriptorSet set, u32 binding, vk::DescriptorType type,
                          const vk::DescriptorImageInfo& image) {
        return vk::WriteDescriptorSet{
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = &image,
        };
    };
    std::vector<vk::WriteDescriptorSet> writes{
        write(g_compress_set, 0, Sampled, infos[0]), write(g_compress_set, 1, Sampled, infos[1]),
        write(g_compress_set, 3, Storage, infos[2]), write(g_tile_set, 0, Combined, infos[0]),
        write(g_tile_set, 1, Storage, infos[3]),     write(g_neighbor_set, 0, Combined, infos[3]),
        write(g_neighbor_set, 1, Storage, infos[4]),
    };
    for (u32 i = 0; i < g_blur_sets.size(); i++) {
        const vk::DescriptorSet set = g_blur_sets[i];
        writes.push_back(write(set, 0, Sampled, infos[i == 0 ? 5 : 6]));
        writes.push_back(write(set, 1, Sampled, infos[4]));
        writes.push_back(write(set, 2, Sampled, infos[2]));
        writes.push_back(write(set, 5, Sampled, infos[1]));
        writes.push_back(write(set, 6, Sampled, infos[7]));
    }
    g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
    return true;
}

void Record(vk::CommandBuffer cmd, u64 address) {
    const auto& dl = g_device->dispatch;
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Output = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr auto Write = vk::AccessFlagBits2::eShaderStorageWrite;
    constexpr auto Read = vk::AccessFlagBits2::eShaderSampledRead;
    constexpr auto AttachmentWrite = vk::AccessFlagBits2::eColorAttachmentWrite;
    constexpr auto General = vk::ImageLayout::eGeneral;
    constexpr auto ShaderRead = vk::ImageLayout::eShaderReadOnlyOptimal;
    constexpr auto Attachment = vk::ImageLayout::eColorAttachmentOptimal;
    // The last frame's reconstruction read the images.
    for (const Image* image : {&g_compressed, &g_tile_max, &g_neighbor_max}) {
        ImageBarrier(cmd, image->image, vk::ImageLayout::eUndefined, General, Fragment | Compute,
                     vk::AccessFlagBits2::eNone, Compute, Write);
    }
    const u64 compress_constants = address + Scene::MotionBlurOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_compress.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_compress.layout, 0, g_compress_set,
                           {}, dl);
    cmd.pushConstants(g_compress.layout, vk::ShaderStageFlagBits::eCompute, 0,
                      sizeof(compress_constants), &compress_constants, dl);
    const vk::Extent2D extent = g_compressed.extent;
    cmd.dispatch((extent.width + GroupPixels - 1) / GroupPixels,
                 (extent.height + GroupPixels - 1) / GroupPixels, 1, dl);
    const u64 tile_constants = compress_constants + TileConstantsOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_tile.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_tile.layout, 0, g_tile_set, {}, dl);
    cmd.pushConstants(g_tile.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(tile_constants),
                      &tile_constants, dl);
    cmd.dispatch(Tiles.width, Tiles.height, 1, dl);
    ImageBarrier(cmd, g_tile_max.image, General, General, Compute, Write, Compute, Read);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_neighbor.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_neighbor.layout, 0, g_neighbor_set,
                           {}, dl);
    cmd.dispatch(Tiles.width, Tiles.height, 1, dl);
    for (const Image* image : {&g_compressed, &g_neighbor_max}) {
        ImageBarrier(cmd, image->image, General, General, Compute, Write, Fragment, Read);
    }
    // Into the second image, then back into the scene.
    ImageBarrier(cmd, g_second.image, vk::ImageLayout::eUndefined, Attachment, Fragment,
                 vk::AccessFlagBits2::eNone, Output, AttachmentWrite);
    Reconstruct(cmd, g_blur_sets[0], g_second.view, address);
    ImageBarrier(cmd, g_second.image, Attachment, ShaderRead, Output, AttachmentWrite, Fragment,
                 Read);
    ImageBarrier(cmd, g_scene, ShaderRead, Attachment, Fragment | Compute, Read, Output,
                 AttachmentWrite);
    Reconstruct(cmd, g_blur_sets[1], g_scene_view, address);
    ImageBarrier(cmd, g_scene, Attachment, ShaderRead, Output, AttachmentWrite, Fragment | Compute,
                 Read);
}

} // namespace Gr2Port::MotionBlur
