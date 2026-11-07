// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "video_core/gr2port/fog.h"
#include "video_core/gr2port/glass.h"
#include "video_core/gr2port/look.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/shaders/glass_frag.h"
#include "video_core/gr2port/shaders/glass_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"

namespace Gr2Port::Glass {

namespace {

struct GlassPush {
    u64 draws;
    u64 materials;
    u64 light;
};

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr vk::ImageSubresourceRange DepthRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
// The scene's colour under the fog, which the panes refract.
Image g_behind;
bool g_behind_ready = false;
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Pipeline g_pipeline;

} // namespace

bool Start(const Device& device, Memory& memory, vk::Format scene_format, vk::Format depth_format) {
    g_device = &device;
    g_memory = &memory;
    const auto& dl = device.dispatch;
    constexpr auto Combined = vk::DescriptorType::eCombinedImageSampler;
    const vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    g_sampler = device.device.createSampler(sampler_info, nullptr, dl).value;
    const vk::DescriptorSetLayoutBinding binding{0, Combined, 1, vk::ShaderStageFlagBits::eFragment,
                                                 &g_sampler};
    g_layout =
        device.device
            .createDescriptorSetLayout({.bindingCount = 1, .pBindings = &binding}, nullptr, dl)
            .value;
    const vk::DescriptorPoolSize size{Combined, 1};
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
    const std::array set_layouts{Textures::SetLayout(), Shadows::SetLayout(), g_layout};
    g_pipeline = CreateGraphicsPipeline(device, {
                                                    .vertex = GLASS_VERT,
                                                    .fragment = GLASS_FRAG,
                                                    .color = scene_format,
                                                    .depth = depth_format,
                                                    .cull = vk::CullModeFlagBits::eBack,
                                                    .depth_test = true,
                                                    .push_constant_bytes = sizeof(GlassPush),
                                                    .set_layouts = set_layouts,
                                                });
    return !!g_pipeline.pipeline;
}

bool Resize(vk::Extent2D extent) {
    g_memory->Destroy(g_behind);
    g_behind = g_memory->CreateImage(Look::SceneFormat, extent,
                                     vk::ImageUsageFlagBits::eColorAttachment |
                                         vk::ImageUsageFlagBits::eSampled |
                                         vk::ImageUsageFlagBits::eTransferDst,
                                     vk::ImageAspectFlagBits::eColor);
    if (!g_behind.image) {
        return false;
    }
    const vk::DescriptorImageInfo info{
        .imageView = g_behind.view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = g_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
    g_behind_ready = false;
    return true;
}

vk::ImageView BehindView() {
    return g_behind.view;
}

void Prepare(vk::CommandBuffer cmd) {
    if (g_behind_ready) {
        return;
    }
    g_behind_ready = true;
    const auto& dl = g_device->dispatch;
    constexpr vk::PipelineStageFlags2 Copy = vk::PipelineStageFlagBits2::eCopy;
    const vk::ImageMemoryBarrier2 to_clear{
        .dstStageMask = Copy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = g_behind.image,
        .subresourceRange = ColorRange,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear}, dl);
    cmd.clearColorImage(g_behind.image, vk::ImageLayout::eTransferDstOptimal,
                        vk::ClearColorValue{.float32 = std::array<float, 4>{}}, ColorRange, dl);
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = Copy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = g_behind.image,
        .subresourceRange = ColorRange,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read}, dl);
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, const Image& depth, vk::Extent2D extent) {
    constexpr Scene::DrawList List = Scene::DrawList::Glass;
    const u32 count = header.draw_commands[static_cast<u32>(List)];
    if (count == 0 && header.effects.background == 0) {
        return;
    }
    const auto& dl = g_device->dispatch;
    constexpr vk::PipelineStageFlags2 Output = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    constexpr vk::PipelineStageFlags2 Copy = vk::PipelineStageFlagBits2::eCopy;
    constexpr vk::PipelineStageFlags2 Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr vk::PipelineStageFlags2 DepthTests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                                   vk::PipelineStageFlagBits2::eLateFragmentTests;
    constexpr vk::AccessFlags2 ColorWrite = vk::AccessFlagBits2::eColorAttachmentWrite;
    constexpr vk::AccessFlags2 ColorAccess =
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    constexpr vk::AccessFlags2 DepthAccess = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    constexpr auto ColorLayout = vk::ImageLayout::eColorAttachmentOptimal;
    constexpr auto SourceLayout = vk::ImageLayout::eTransferSrcOptimal;
    constexpr auto TargetLayout = vk::ImageLayout::eTransferDstOptimal;
    constexpr auto DepthLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    constexpr auto DepthReadLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    const vk::Image scene = Look::SceneImage();

    // The scene so far to the copy. The fog reads the depth, and the last frame's panes sampled
    // the copy.
    const std::array<vk::ImageMemoryBarrier2, 3> to_copy{{
        {.srcStageMask = Output,
         .srcAccessMask = ColorWrite,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
         .oldLayout = ColorLayout,
         .newLayout = SourceLayout,
         .image = scene,
         .subresourceRange = ColorRange},
        {.srcStageMask = Fragment,
         .dstStageMask = Copy,
         .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .oldLayout = vk::ImageLayout::eUndefined,
         .newLayout = TargetLayout,
         .image = g_behind.image,
         .subresourceRange = ColorRange},
        {.srcStageMask = DepthTests,
         .srcAccessMask = DepthAccess,
         .dstStageMask = Fragment,
         .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
         .oldLayout = DepthLayout,
         .newLayout = DepthReadLayout,
         .image = depth.image,
         .subresourceRange = DepthRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(to_copy.size()),
                          .pImageMemoryBarriers = to_copy.data()},
                         dl);
    const vk::ImageCopy region{
        .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .extent = vk::Extent3D{extent.width, extent.height, 1},
    };
    cmd.copyImage(scene, SourceLayout, g_behind.image, TargetLayout, region, dl);

    // The fog over the copy.
    const std::array<vk::ImageMemoryBarrier2, 2> copied{{
        {.srcStageMask = Copy,
         .dstStageMask = Output,
         .dstAccessMask = ColorAccess,
         .oldLayout = SourceLayout,
         .newLayout = ColorLayout,
         .image = scene,
         .subresourceRange = ColorRange},
        {.srcStageMask = Copy,
         .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
         .dstStageMask = Output,
         .dstAccessMask = ColorAccess,
         .oldLayout = TargetLayout,
         .newLayout = ColorLayout,
         .image = g_behind.image,
         .subresourceRange = ColorRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(copied.size()),
                          .pImageMemoryBarriers = copied.data()},
                         dl);
    const vk::RenderingAttachmentInfo behind{
        .imageView = g_behind.view,
        .imageLayout = ColorLayout,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &behind},
                       dl);
    Fog::RecordApply(cmd, address, extent);
    cmd.endRendering(dl);

    // The panes, into the scene.
    const std::array<vk::ImageMemoryBarrier2, 2> to_panes{{
        {.srcStageMask = Output,
         .srcAccessMask = ColorWrite,
         .dstStageMask = Fragment,
         .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
         .oldLayout = ColorLayout,
         .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
         .image = g_behind.image,
         .subresourceRange = ColorRange},
        {.srcStageMask = Fragment,
         .dstStageMask = DepthTests,
         .dstAccessMask = DepthAccess,
         .oldLayout = DepthReadLayout,
         .newLayout = DepthLayout,
         .image = depth.image,
         .subresourceRange = DepthRange},
    }};
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(to_panes.size()),
                          .pImageMemoryBarriers = to_panes.data()},
                         dl);
    if (count == 0) {
        return;
    }
    const vk::RenderingAttachmentInfo scene_attachment{
        .imageView = Look::SceneView(),
        .imageLayout = ColorLayout,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth.view,
        .imageLayout = DepthLayout,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &scene_attachment,
                        .pDepthAttachment = &depth_attachment},
                       dl);
    // A negative height keeps the game's y-up clip space.
    const vk::Viewport viewport{
        .x = 0.0f,
        .y = static_cast<float>(extent.height),
        .width = static_cast<float>(extent.width),
        .height = -static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    const std::array sets{Textures::Set(), Shadows::Set(), g_set};
    const GlassPush push{
        .draws = address + Scene::DrawsOffset,
        .materials = address + Scene::GlassMaterialsOffset,
        .light = address + Scene::LightOffset,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_pipeline.layout, 0, sets, {}, dl);
    cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                      &push, dl);
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    cmd.drawIndexedIndirect(buffer,
                            offset + Scene::CommandsOffset +
                                u64{Scene::FirstCommand(header, List)} * sizeof(Scene::Command),
                            count, sizeof(Scene::Command), dl);
    cmd.endRendering(dl);
}

} // namespace Gr2Port::Glass
