// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>

#include "common/logging/log.h"
#include "video_core/gr2port/look.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/scene.h"
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

// The game's luminance pass reads one pixel of each 8x8 tile in groups of 8x8 tiles.
constexpr u32 TilePixels = 8;
constexpr u32 GroupPixels = 64;
// The offset of the pixel in its tile runs through this table in each direction, the x offset
// faster, for 32 frames.
constexpr std::array<u32, 8> TileOffsets{0, 7, 6, 3, 1, 5, 2, 4};
constexpr u32 TileOffsetFrames = 32;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
Image g_scene;
vk::Extent2D g_extent;
vk::Sampler g_scene_sampler;
vk::DescriptorSetLayout g_scene_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_scene_set;
Pipeline g_sky;
Pipeline g_luminance;
Pipeline g_exposure;
Pipeline g_composite;
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
    const vk::DescriptorSetLayoutBinding binding{
        0, vk::DescriptorType::eCombinedImageSampler, 1,
        vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eCompute, &g_scene_sampler};
    g_scene_layout =
        d.device.createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding}, nullptr, dl)
            .value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 1};
    g_pool = d.device
                 .createDescriptorPool({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size},
                                       nullptr, dl)
                 .value;
    if (!g_scene_sampler || !g_scene_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_scene_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_scene_set = sets.front();
    return true;
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
    return g_sky.pipeline && g_luminance.pipeline && g_exposure.pipeline;
}

bool Resize(vk::Extent2D extent, vk::Format picture_format) {
    const auto& dl = g_device->dispatch;
    g_memory->Destroy(g_scene);
    g_scene = g_memory->CreateImage(SceneFormat, extent,
                                    vk::ImageUsageFlagBits::eColorAttachment |
                                        vk::ImageUsageFlagBits::eSampled,
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
    if (g_picture_format != picture_format) {
        const std::array scene_set{g_scene_layout};
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
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_composite.layout, 0, g_scene_set, {},
                           dl);
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
