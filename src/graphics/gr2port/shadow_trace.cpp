// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/point_lights.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/shaders/fullscreen_vert.h"
#include "graphics/gr2port/shaders/shadow_trace_frag.h"
#include "graphics/gr2port/shadow_trace.h"

namespace Gr2Port::ShadowTrace {

namespace {

// The program exports two half floats; the light pass reads the first.
constexpr vk::Format Format = vk::Format::eR16G16Sfloat;

struct Push {
    u64 constants;
    u64 point_lights;
    u64 light_grid;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Pipeline g_pipeline;
Image g_image;
// The image holds no shadow until the first trace.
bool g_cleared = false;

void ImageBarrier(vk::CommandBuffer cmd, vk::ImageLayout from, vk::ImageLayout to,
                  vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                  vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = g_image.image,
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
    const vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                                 vk::ShaderStageFlagBits::eFragment, &g_sampler};
    g_layout =
        device.device
            .createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding}, nullptr, dl)
            .value;
    const vk::DescriptorPoolSize size{vk::DescriptorType::eCombinedImageSampler, 1};
    g_pool = device.device
                 .createDescriptorPool({.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size},
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
    g_pipeline = CreateGraphicsPipeline(device, {
                                                    .vertex = FULLSCREEN_VERT,
                                                    .fragment = SHADOW_TRACE_FRAG,
                                                    .color = Format,
                                                    .push_constant_bytes = sizeof(Push),
                                                    .set_layouts = set_layouts,
                                                });
    return static_cast<bool>(g_pipeline.pipeline);
}

bool Resize(vk::Extent2D extent, vk::ImageView depth) {
    g_memory->Destroy(g_image);
    g_image = g_memory->CreateImage(Format, extent,
                                    vk::ImageUsageFlagBits::eColorAttachment |
                                        vk::ImageUsageFlagBits::eSampled |
                                        vk::ImageUsageFlagBits::eTransferDst,
                                    vk::ImageAspectFlagBits::eColor);
    if (!g_image.image) {
        return false;
    }
    g_cleared = false;
    const vk::DescriptorImageInfo depth_info{
        .imageView = depth,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = g_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &depth_info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
    return true;
}

vk::ImageView View() {
    return g_image.view;
}

void Record(vk::CommandBuffer cmd, u64 address, bool traced) {
    const auto& dl = g_device->dispatch;
    constexpr auto Readers = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Read = vk::AccessFlagBits2::eShaderSampledRead;
    if (!g_cleared) {
        g_cleared = true;
        ImageBarrier(cmd, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                     vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
                     vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite);
        cmd.clearColorImage(g_image.image, vk::ImageLayout::eTransferDstOptimal,
                            vk::ClearColorValue{.float32 = std::array<float, 4>{}},
                            vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
                            dl);
        ImageBarrier(cmd, vk::ImageLayout::eTransferDstOptimal,
                     vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eClear,
                     vk::AccessFlagBits2::eTransferWrite, Readers, Read);
    }
    if (!traced) {
        return;
    }
    // The last frame's light pass read the image.
    ImageBarrier(cmd, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eColorAttachmentOptimal, Readers, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                 vk::AccessFlagBits2::eColorAttachmentWrite);
    const vk::RenderingAttachmentInfo color{
        .imageView = g_image.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eDontCare,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, g_image.extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color},
                       dl);
    cmd.setViewport(0,
                    vk::Viewport{.width = static_cast<float>(g_image.extent.width),
                                 .height = static_cast<float>(g_image.extent.height),
                                 .maxDepth = 1.0f},
                    dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, g_image.extent}, dl);
    const Push push{
        .constants = address + Scene::ShadowTraceOffset,
        .point_lights = address + Scene::PointLightsOffset,
        .light_grid = PointLights::GridAddress(),
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_pipeline.layout, 0, g_set, {}, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                      &push, dl);
    cmd.draw(3, 1, 0, 0, dl);
    cmd.endRendering(dl);
    ImageBarrier(cmd, vk::ImageLayout::eColorAttachmentOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                 vk::AccessFlagBits2::eColorAttachmentWrite, Readers, Read);
}

} // namespace Gr2Port::ShadowTrace
