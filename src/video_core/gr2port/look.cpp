// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include "common/logging/log.h"
#include "video_core/gr2port/look.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/shaders/bloom_blur_frag.h"
#include "video_core/gr2port/shaders/bloom_extract_frag.h"
#include "video_core/gr2port/shaders/composite_frag.h"
#include "video_core/gr2port/shaders/exposure_comp.h"
#include "video_core/gr2port/shaders/fullscreen_vert.h"
#include "video_core/gr2port/shaders/luminance_comp.h"
#include "video_core/gr2port/shaders/sky_frag.h"
#include "video_core/gr2port/textures.h"

namespace Gr2Port::Look {

namespace {

// shaders/scene.glsl's Meter: the sum of log luminance, the exposure and the adapted brightness.
struct MeterState {
    s32 log_sum;
    float exposure;
    float adapted;
    u32 padding;
};

struct LuminancePush {
    u64 meter;
    std::array<u32, 2> size;
    std::array<u32, 2> offset;
};

struct ExposurePush {
    u64 grade;
    u64 meter;
    float tile_share;
    u32 padding;
};

struct CompositePush {
    u64 grade;
    u64 meter;
};

struct ExtractPush {
    u64 grade;
    std::array<float, 2> scene_pixel;
};

struct BlurPush {
    u64 grade;
    std::array<float, 2> step;
    u32 feedback;
    u32 padding;
};

// The bloom's images, a quarter of the scene's size, in a ring: a frame's bright pass and second
// blur write the current one, its first blur the next, and the last frame's result is the third.
constexpr u32 BloomImages = 3;
constexpr u32 BloomDivisor = 4;
// The sets: the scene's colour; for each image as the current one, the composite's, the first
// blur's and the second blur's; and the bright pass's.
constexpr u32 Sets = 1 + 3 * BloomImages + 1;

// The game's luminance pass reads one pixel of each 8x8 tile in groups of 8x8 tiles.
constexpr u32 TilePixels = 8;
constexpr u32 GroupPixels = 64;
// The offset of the pixel in its tile runs through this table in each direction, the x offset
// faster, for 32 frames.
constexpr std::array<u32, 8> TileOffsets{0, 7, 6, 3, 1, 5, 2, 4};
constexpr u32 TileOffsetFrames = 32;
// The word of a character's pixel constants that holds its self-translucency.
constexpr u32 SelfTranslucencyConstant = 0x87;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
Image g_scene;
vk::Extent2D g_extent;
vk::Sampler g_scene_sampler;
vk::Sampler g_depth_sampler;
vk::DescriptorSetLayout g_scene_layout;
// Two images with their own samplers, for the bloom and the composite.
vk::DescriptorSetLayout g_pair_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_scene_set;
vk::DescriptorSet g_extract_set;
// Each indexed by the current bloom image.
std::array<vk::DescriptorSet, BloomImages> g_composite_sets;
std::array<vk::DescriptorSet, BloomImages> g_across_sets;
std::array<vk::DescriptorSet, BloomImages> g_down_sets;
std::array<Image, BloomImages> g_bloom;
bool g_bloom_cleared = false;
// The image that holds the last bloom, and whether the last frame ran bloom.
u32 g_bloom_image = 0;
bool g_bloom_ran = false;
Pipeline g_sky;
Pipeline g_luminance;
Pipeline g_exposure;
Pipeline g_composite;
Pipeline g_extract;
Pipeline g_blur;
vk::Format g_picture_format = vk::Format::eUndefined;
Buffer g_meter;
Buffer g_meter_read;
bool g_meter_cleared = false;
u32 g_meter_frame = 0;

bool CreateSceneSet(const Device& d) {
    const auto& dl = d.dispatch;
    const vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_scene_sampler = d.device.createSampler(sampler_info, nullptr, dl).value;
    vk::SamplerCreateInfo depth_info = sampler_info;
    depth_info.magFilter = vk::Filter::eNearest;
    depth_info.minFilter = vk::Filter::eNearest;
    g_depth_sampler = d.device.createSampler(depth_info, nullptr, dl).value;
    const vk::DescriptorSetLayoutBinding binding{
        0, vk::DescriptorType::eCombinedImageSampler, 1,
        vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute, &g_scene_sampler};
    g_scene_layout =
        d.device.createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding}, nullptr, dl)
            .value;
    const std::array<vk::DescriptorSetLayoutBinding, 2> pair{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
        {1, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment},
    }};
    g_pair_layout = d.device
                        .createDescriptorSetLayout({.bindingCount = static_cast<u32>(pair.size()),
                                                    .pBindings = pair.data()},
                                                   nullptr, dl)
                        .value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 2 * Sets};
    g_pool = d.device
                 .createDescriptorPool({.maxSets = Sets, .poolSizeCount = 1, .pPoolSizes = &size},
                                       nullptr, dl)
                 .value;
    if (!g_scene_sampler || !g_depth_sampler || !g_scene_layout || !g_pair_layout || !g_pool) {
        return false;
    }
    std::array<vk::DescriptorSetLayout, Sets> layouts;
    layouts.fill(g_pair_layout);
    layouts[0] = g_scene_layout;
    const auto [result, sets] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = Sets, .pSetLayouts = layouts.data()}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_scene_set = sets[0];
    for (u32 i = 0; i < BloomImages; i++) {
        g_composite_sets[i] = sets[1 + i];
        g_across_sets[i] = sets[1 + BloomImages + i];
        g_down_sets[i] = sets[1 + 2 * BloomImages + i];
    }
    g_extract_set = sets[Sets - 1];
    return true;
}

// Points a pair set's bindings at two images, each read with a sampler.
void WritePair(vk::DescriptorSet set, vk::ImageView first, vk::Sampler first_sampler,
               vk::ImageLayout first_layout, vk::ImageView second, vk::Sampler second_sampler,
               vk::ImageLayout second_layout) {
    const std::array<vk::DescriptorImageInfo, 2> infos{{
        {first_sampler, first, first_layout},
        {second_sampler, second, second_layout},
    }};
    std::array<vk::WriteDescriptorSet, 2> writes;
    for (u32 b = 0; b < writes.size(); b++) {
        writes[b] = {
            .dstSet = set,
            .dstBinding = b,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &infos[b],
        };
    }
    g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
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

// Draws one full-screen bloom pass into target and leaves it for fragment shaders to read.
void BloomPass(vk::CommandBuffer cmd, const Image& target, const Pipeline& pipeline,
               vk::DescriptorSet set, const void* push, u32 push_bytes) {
    const auto& dl = g_device->dispatch;
    ImageBarrier(cmd, target.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eColorAttachmentOptimal,
                 vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                 vk::AccessFlagBits2::eColorAttachmentWrite);
    const vk::RenderingAttachmentInfo color{
        .imageView = target.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, target.extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color},
                       dl);
    const vk::Viewport viewport{
        .width = static_cast<float>(target.extent.width),
        .height = static_cast<float>(target.extent.height),
        .maxDepth = 1.0f,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, set, {}, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, target.extent}, dl);
    cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, push_bytes, push,
                      dl);
    cmd.draw(3, 1, 0, 0, dl);
    cmd.endRendering(dl);
    ImageBarrier(
        cmd, target.image, vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
        vk::AccessFlagBits2::eShaderSampledRead);
}

void MeterBarrier(vk::CommandBuffer cmd, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier},
                         g_device->dispatch);
}

} // namespace

bool Start(const Device& device, Memory& memory, vk::Format depth_format) {
    g_device = &device;
    g_memory = &memory;
    if (!CreateSceneSet(device)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the scene colour's set failed");
        return false;
    }
    g_meter = memory.CreateBuffer(sizeof(MeterState),
                                  vk::BufferUsageFlagBits::eStorageBuffer |
                                      vk::BufferUsageFlagBits::eTransferDst |
                                      vk::BufferUsageFlagBits::eTransferSrc,
                                  Placement::Device);
    g_meter_read = memory.CreateBuffer(sizeof(MeterState), vk::BufferUsageFlagBits::eTransferDst,
                                       Placement::Readback);
    if (!g_meter.buffer || g_meter_read.mapped == nullptr) {
        return false;
    }
    std::memset(g_meter_read.mapped, 0, sizeof(MeterState));
    const std::array texture_set{Textures::SetLayout()};
    const std::array scene_set{g_scene_layout};
    g_sky = CreateGraphicsPipeline(device, {
                                               .vertex = FULLSCREEN_VERT,
                                               .fragment = SKY_FRAG,
                                               .color = SceneFormat,
                                               .depth = depth_format,
                                               .depth_test = true,
                                               .depth_compare = vk::CompareOp::eGreaterOrEqual,
                                               .push_constant_bytes = sizeof(u64),
                                               .set_layouts = texture_set,
                                           });
    g_luminance = CreateComputePipeline(device, LUMINANCE_COMP, sizeof(LuminancePush), scene_set);
    g_exposure = CreateComputePipeline(device, EXPOSURE_COMP, sizeof(ExposurePush));
    const std::array pair_set{g_pair_layout};
    g_extract = CreateGraphicsPipeline(device, {
                                                   .vertex = FULLSCREEN_VERT,
                                                   .fragment = BLOOM_EXTRACT_FRAG,
                                                   .color = SceneFormat,
                                                   .push_constant_bytes = sizeof(ExtractPush),
                                                   .set_layouts = pair_set,
                                               });
    g_blur = CreateGraphicsPipeline(device, {
                                                .vertex = FULLSCREEN_VERT,
                                                .fragment = BLOOM_BLUR_FRAG,
                                                .color = SceneFormat,
                                                .push_constant_bytes = sizeof(BlurPush),
                                                .set_layouts = pair_set,
                                            });
    return g_sky.pipeline && g_luminance.pipeline && g_exposure.pipeline && g_extract.pipeline &&
           g_blur.pipeline;
}

bool Resize(vk::Extent2D extent, vk::Format picture_format, vk::ImageView depth) {
    const auto& dl = g_device->dispatch;
    g_memory->Destroy(g_scene);
    g_scene = g_memory->CreateImage(SceneFormat, extent,
                                    vk::ImageUsageFlagBits::eColorAttachment |
                                        vk::ImageUsageFlagBits::eSampled |
                                        vk::ImageUsageFlagBits::eTransferSrc,
                                    vk::ImageAspectFlagBits::eColor);
    if (!g_scene.image) {
        return false;
    }
    g_extent = extent;
    const vk::DescriptorImageInfo info{
        .imageView = g_scene.view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = g_scene_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, dl);
    const vk::Extent2D bloom_extent{std::max(extent.width / BloomDivisor, 1u),
                                    std::max(extent.height / BloomDivisor, 1u)};
    for (Image& image : g_bloom) {
        g_memory->Destroy(image);
        image = g_memory->CreateImage(SceneFormat, bloom_extent,
                                      vk::ImageUsageFlagBits::eColorAttachment |
                                          vk::ImageUsageFlagBits::eSampled |
                                          vk::ImageUsageFlagBits::eTransferDst,
                                      vk::ImageAspectFlagBits::eColor);
        if (!image.image) {
            return false;
        }
    }
    g_bloom_cleared = false;
    g_bloom_ran = false;
    constexpr auto Read = vk::ImageLayout::eShaderReadOnlyOptimal;
    WritePair(g_extract_set, g_scene.view, g_scene_sampler, Read, depth, g_depth_sampler,
              vk::ImageLayout::eDepthStencilReadOnlyOptimal);
    for (u32 i = 0; i < BloomImages; i++) {
        const vk::ImageView current = g_bloom[i].view;
        const vk::ImageView next = g_bloom[(i + 1) % BloomImages].view;
        const vk::ImageView last = g_bloom[(i + 2) % BloomImages].view;
        WritePair(g_composite_sets[i], g_scene.view, g_scene_sampler, Read, current,
                  g_scene_sampler, Read);
        WritePair(g_across_sets[i], current, g_scene_sampler, Read, last, g_scene_sampler, Read);
        WritePair(g_down_sets[i], next, g_scene_sampler, Read, last, g_scene_sampler, Read);
    }
    if (g_picture_format != picture_format) {
        const std::array scene_set{g_pair_layout};
        DestroyPipeline(*g_device, g_composite);
        g_composite =
            CreateGraphicsPipeline(*g_device, {
                                                  .vertex = FULLSCREEN_VERT,
                                                  .fragment = COMPOSITE_FRAG,
                                                  .color = picture_format,
                                                  .push_constant_bytes = sizeof(CompositePush),
                                                  .set_layouts = scene_set,
                                              });
        g_picture_format = picture_format;
    }
    return static_cast<bool>(g_composite.pipeline);
}

vk::Image SceneImage() {
    return g_scene.image;
}

vk::ImageView SceneView() {
    return g_scene.view;
}

void RecordSky(vk::CommandBuffer cmd, u64 packet_address) {
    const auto& dl = g_device->dispatch;
    // A negative height keeps the game's y-up clip space, as in the scene's draws.
    const vk::Viewport viewport{
        .x = 0.0f,
        .y = static_cast<float>(g_extent.height),
        .width = static_cast<float>(g_extent.width),
        .height = -static_cast<float>(g_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const u64 sky = packet_address + Scene::SkyOffset;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_sky.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_sky.layout, 0, Textures::Set(), {},
                           dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, g_extent}, dl);
    cmd.pushConstants(g_sky.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(sky), &sky,
                      dl);
    cmd.draw(3, 1, 0, 0, dl);
}

void RecordMeter(vk::CommandBuffer cmd, u64 packet_address) {
    const auto& dl = g_device->dispatch;
    if (!g_meter_cleared) {
        g_meter_cleared = true;
        cmd.fillBuffer(g_meter.buffer, 0, sizeof(MeterState), 0, dl);
        MeterBarrier(cmd, vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
                     vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageRead |
                         vk::AccessFlagBits2::eShaderStorageWrite);
    }
    g_meter_frame = (g_meter_frame + 1) % TileOffsetFrames;
    const LuminancePush luminance{
        .meter = g_meter.address,
        .size = {g_extent.width, g_extent.height},
        .offset = {TileOffsets[g_meter_frame % TileOffsets.size()],
                   TileOffsets[g_meter_frame / TileOffsets.size()]},
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_luminance.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_luminance.layout, 0, g_scene_set, {},
                           dl);
    cmd.pushConstants(g_luminance.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(luminance),
                      &luminance, dl);
    cmd.dispatch((g_extent.width + GroupPixels - 1) / GroupPixels,
                 (g_extent.height + GroupPixels - 1) / GroupPixels, 1, dl);
    MeterBarrier(
        cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
    const u32 tiles = ((g_extent.width + TilePixels - 1) / TilePixels) *
                      ((g_extent.height + TilePixels - 1) / TilePixels);
    const ExposurePush exposure{
        .grade = packet_address + Scene::GradeOffset,
        .meter = g_meter.address,
        .tile_share = 1.0f / static_cast<float>(tiles),
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_exposure.pipeline, dl);
    cmd.pushConstants(g_exposure.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(exposure),
                      &exposure, dl);
    cmd.dispatch(1, 1, 1, dl);
    // Also covers the next frame's measure of the sum this program clears.
    MeterBarrier(
        cmd, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageWrite,
        vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eComputeShader,
        vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite);
}

void RecordBloom(vk::CommandBuffer cmd, u32 bloom, u64 packet_address) {
    const auto& dl = g_device->dispatch;
    if (!g_bloom_cleared) {
        g_bloom_cleared = true;
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        for (const Image& image : g_bloom) {
            ImageBarrier(cmd, image.image, vk::ImageLayout::eUndefined,
                         vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eNone,
                         vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eClear,
                         vk::AccessFlagBits2::eTransferWrite);
            cmd.clearColorImage(image.image, vk::ImageLayout::eTransferDstOptimal,
                                vk::ClearColorValue{.float32 = std::array<float, 4>{}}, range, dl);
            ImageBarrier(cmd, image.image, vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
                         vk::PipelineStageFlagBits2::eFragmentShader,
                         vk::AccessFlagBits2::eShaderSampledRead);
        }
    }
    if ((bloom & Scene::BloomOn) == 0) {
        g_bloom_ran = false;
        return;
    }
    // As the game: the image after the last bloom's, or the first after a frame without bloom.
    const u32 current = g_bloom_ran ? (g_bloom_image + 1) % BloomImages : 0;
    const u32 next = (current + 1) % BloomImages;
    const u64 grade = packet_address + Scene::GradeOffset;
    const ExtractPush extract{
        .grade = grade,
        .scene_pixel = {1.0f / static_cast<float>(g_extent.width),
                        1.0f / static_cast<float>(g_extent.height)},
    };
    BloomPass(cmd, g_bloom[current], g_extract, g_extract_set, &extract, sizeof(extract));
    const vk::Extent2D size = g_bloom[current].extent;
    const BlurPush across{
        .grade = grade,
        .step = {1.0f / static_cast<float>(size.width), 0.0f},
    };
    BloomPass(cmd, g_bloom[next], g_blur, g_across_sets[current], &across, sizeof(across));
    const BlurPush down{
        .grade = grade,
        .step = {0.0f, 1.0f / static_cast<float>(size.height)},
        .feedback = g_bloom_ran && (bloom & Scene::BloomFeedback) != 0 ? 1u : 0u,
    };
    BloomPass(cmd, g_bloom[current], g_blur, g_down_sets[current], &down, sizeof(down));
    g_bloom_image = current;
    g_bloom_ran = true;
}

void RecordComposite(vk::CommandBuffer cmd, u64 packet_address) {
    const auto& dl = g_device->dispatch;
    const vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(g_extent.width),
        .height = static_cast<float>(g_extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const CompositePush push{
        .grade = packet_address + Scene::GradeOffset,
        .meter = g_meter.address,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_composite.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_composite.layout, 0,
                           g_composite_sets[g_bloom_image], {}, dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, g_extent}, dl);
    cmd.pushConstants(g_composite.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                      &push, dl);
    cmd.draw(3, 1, 0, 0, dl);
}

void LogAndReadMeter(vk::CommandBuffer cmd, const u8* packet) {
    const auto& dl = g_device->dispatch;
    Scene::Header header;
    std::memcpy(&header, packet, sizeof(header));
    Scene::Sky sky;
    std::memcpy(&sky, packet + Scene::SkyOffset, sizeof(sky));
    Scene::Grade grade;
    std::memcpy(&grade, packet + Scene::GradeOffset, sizeof(grade));
    MeterState meter;
    std::memcpy(&meter, g_meter_read.mapped, sizeof(meter));
    const float* knots = grade.knots[0];
    LOG_INFO(Render_Vulkan,
             "GR2port look: exposure {:.3f} stops, range {:.3f}, speed {:.4f}, key {:.3f}; last "
             "read exposure {:.3f}, adapted {:.4f}; saturation {:.3f}, lens {}; R curve {:.3f} "
             "{:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}",
             grade.exposure, grade.adaption_range, grade.adaption_speed, grade.key_value,
             meter.exposure, meter.adapted, grade.saturation, grade.distorted, knots[0], knots[1],
             knots[2], knots[3], knots[4], knots[5], knots[6]);
    LOG_INFO(Render_Vulkan,
             "GR2port bloom: flags {}, low ({:.3f} {:.3f} {:.3f} {:.3f}), high ({:.3f} {:.3f} "
             "{:.3f} {:.3f}), correction ({:.3f} {:.3f} {:.3f} {:.3f}), depths ({:.3f} {:.3f} "
             "{:.3f} {:.3f}), strength ({:.3f} {:.3f} {:.3f}), width ({:.3f} {:.3f}), feedback "
             "{:.3f}",
             header.bloom, grade.bloom_low[0], grade.bloom_low[1], grade.bloom_low[2],
             grade.bloom_low[3], grade.bloom_high[0], grade.bloom_high[1], grade.bloom_high[2],
             grade.bloom_high[3], grade.bloom_correction[0], grade.bloom_correction[1],
             grade.bloom_correction[2], grade.bloom_correction[3], grade.bloom_depths[0],
             grade.bloom_depths[1], grade.bloom_depths[2], grade.bloom_depths[3],
             grade.bloom_strength[0], grade.bloom_strength[1], grade.bloom_strength[2],
             grade.bloom_width[0], grade.bloom_width[1], grade.bloom_feedback);
    if (header.sky == 0) {
        LOG_INFO(Render_Vulkan, "GR2port sky: none, background ({:.3f} {:.3f} {:.3f})",
                 header.background[0], header.background[1], header.background[2]);
    } else {
        LOG_INFO(Render_Vulkan,
                 "GR2port sky: cube maps {} {} (blend {:.3f}), clouds {} opacity {:.3f}, sun "
                 "({:.2f} {:.2f} {:.2f}) size {:.4f} outer {:.2f} direction ({:.3f} {:.3f} {:.3f})",
                 sky.cubes[0], sky.cubes[1], sky.blend, sky.cloud, sky.cloud_color[3],
                 sky.sun_color[0], sky.sun_color[1], sky.sun_color[2], sky.sun_size, sky.sun_outer,
                 sky.sun_direction[0], sky.sun_direction[1], sky.sun_direction[2]);
    }
    Scene::Light light;
    std::memcpy(&light, packet + Scene::LightOffset, sizeof(light));
    const float* projection = light.shadow_projection[0];
    LOG_INFO(Render_Vulkan,
             "GR2port world: ambient cube maps {} {} and {} {} (blend {:.3f}), strength {:.3f}, "
             "{} occluders, toning {:.3f}, gradation from {:.1f} over {:.1f}, background from "
             "{:.1f} over {:.1f} strength {:.3f} level {:.2f}, shadows split at {:.1f} {:.1f}, end "
             "at {:.1f}; first cascade rows ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} "
             "{:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f})",
             light.radiance[0], light.irradiance[0], light.radiance[1], light.irradiance[1],
             light.ibl_blend, light.ibl_intensity, light.occluders, light.toning,
             light.gradation_start, 1.0f / light.gradation_range, light.background_start,
             1.0f / light.background_range, light.background_intensity, light.background_lod,
             light.split_depths[1], light.split_depths[2], light.shadow_fade_start + 10.0f,
             projection[0], projection[1], projection[2], projection[3], projection[4],
             projection[5], projection[6], projection[7], projection[8], projection[9],
             projection[10], projection[11], projection[12], projection[13], projection[14],
             projection[15]);
    u32 without_normal_map = 0;
    u32 without_detail = 0;
    u32 without_reflection = 0;
    u32 translucent = 0;
    for (u32 i = 0; i < header.characters; i++) {
        Scene::Character character;
        std::memcpy(&character, packet + Scene::CharactersOffset + i * sizeof(character),
                    sizeof(character));
        without_normal_map += character.normal_map == 0;
        without_detail += character.detail == 0;
        without_reflection += character.reflection == 0;
        translucent += character.pixel_constants[SelfTranslucencyConstant] != 0;
    }
    LOG_INFO(Render_Vulkan,
             "GR2port characters: {} draws, {} without a normal map, {} without a detail map, {} "
             "without a reflection map, {} with self-translucency",
             header.characters, without_normal_map, without_detail, without_reflection,
             translucent);
    if (!g_meter_cleared) {
        return;
    }
    MeterBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eShaderStorageWrite, vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferRead);
    const vk::BufferCopy region{.size = sizeof(MeterState)};
    cmd.copyBuffer(g_meter.buffer, g_meter_read.buffer, region, dl);
}

} // namespace Gr2Port::Look
