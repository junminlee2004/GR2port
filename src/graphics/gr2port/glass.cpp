// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <span>
#include <vector>

#include "graphics/gr2port/fog.h"
#include "graphics/gr2port/glass.h"
#include "graphics/gr2port/look.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/meshes.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/shaders/glass_frag.h"
#include "graphics/gr2port/shaders/glass_vert.h"
#include "graphics/gr2port/shadows.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/upscaler.h"

namespace Gr2Port::Glass {

namespace {

struct GlassPush {
    u64 draws;
    u64 materials;
    u64 light;
};

constexpr vk::ImageSubresourceRange ColorRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr vk::ImageSubresourceRange DepthRange{vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};

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

// Values of glass.frag's GlassOutput, constant id 0: the share of the scene that shows through,
// and the panes' light.
constexpr u32 ThroughOutput = 1;
constexpr u32 LightOutput = 2;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
// The scene's colour under the fog, which the panes refract.
Image g_behind;
bool g_behind_ready = false;
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
// With refraction, one pass over the copy; without, the pass that multiplies the scene by what
// shows through, then the one that adds the panes' light.
bool g_refraction = true;
std::array<Pipeline, 2> g_pipelines;
u32 g_passes = 0;

// The program with its constant id 0 set to value, as a specialization sets it; the graphics
// pipeline helper takes none. Empty when the program has no such 32-bit constant.
std::vector<u32> Specialized(std::span<const u32> code, u32 value) {
    constexpr u32 OpDecorate = 71;
    constexpr u32 OpSpecConstant = 50;
    constexpr u32 SpecId = 1;
    constexpr std::size_t HeaderWords = 5;
    std::vector<u32> result(code.begin(), code.end());
    u32 target = 0;
    for (std::size_t i = HeaderWords; i < result.size();) {
        const u32 words = result[i] >> 16;
        const u32 opcode = result[i] & 0xffff;
        if (words == 0 || i + words > result.size()) {
            break;
        }
        if (opcode == OpDecorate && words == 4 && result[i + 2] == SpecId && result[i + 3] == 0) {
            target = result[i + 1];
        } else if (opcode == OpSpecConstant && words == 4 && target != 0 &&
                   result[i + 2] == target) {
            result[i + 3] = value;
            return result;
        }
        i += words;
    }
    return {};
}

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
    GraphicsPipelineInfo info{
        .vertex = GLASS_VERT,
        .fragment = GLASS_FRAG,
        .color = scene_format,
        .depth = depth_format,
        .cull = vk::CullModeFlagBits::eBack,
        .depth_test = true,
        .keep_target_alpha = true,
        .push_constant_bytes = sizeof(GlassPush),
        .set_layouts = set_layouts,
    };
    g_refraction = Settings::Get().graphics.glass_refraction;
    if (g_refraction) {
        g_pipelines[0] = CreateGraphicsPipeline(device, info);
        g_passes = 1;
        return !!g_pipelines[0].pipeline;
    }
    // Without refraction the scene in the target is what shows through: the first pass multiplies
    // it by the panes' share and writes their depth, the second adds their light on that depth.
    const std::vector<u32> through = Specialized(GLASS_FRAG, ThroughOutput);
    const std::vector<u32> light = Specialized(GLASS_FRAG, LightOutput);
    if (through.empty() || light.empty()) {
        return false;
    }
    const vk::PipelineColorBlendAttachmentState multiply{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eZero,
        .dstColorBlendFactor = vk::BlendFactor::eSrcColor,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vk::BlendFactor::eOne,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB,
    };
    vk::PipelineColorBlendAttachmentState add = multiply;
    add.srcColorBlendFactor = vk::BlendFactor::eOne;
    add.dstColorBlendFactor = vk::BlendFactor::eOne;
    info.fragment = through;
    info.blend = &multiply;
    g_pipelines[0] = CreateGraphicsPipeline(device, info);
    info.fragment = light;
    info.blend = &add;
    info.depth_write = false;
    info.depth_compare = vk::CompareOp::eGreaterOrEqual;
    g_pipelines[1] = CreateGraphicsPipeline(device, info);
    g_passes = 2;
    return g_pipelines[0].pipeline && g_pipelines[1].pipeline;
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

namespace {

// Copies the scene's colour and draws the fog over the copy, as the game's BackupColorBuffer2
// does. Leaves the copy readable and the scene's colour and depth as attachments.
void RecordCopy(vk::CommandBuffer cmd, u64 address, const Image& depth, vk::Extent2D extent) {
    const auto& dl = g_device->dispatch;
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
}

} // namespace

void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, const Image& depth, vk::Extent2D extent) {
    constexpr Scene::DrawList List = Scene::DrawList::Glass;
    const u32 count = header.draw_commands[static_cast<u32>(List)];
    const bool readers = header.effects.background != 0 || header.effects.water_draws != 0;
    if (count == 0 && !readers) {
        return;
    }
    const auto& dl = g_device->dispatch;
    // Without refraction the panes read no copy; the water and the effects' programs still do.
    if (g_refraction || readers) {
        RecordCopy(cmd, address, depth, extent);
    } else {
        // The panes blend over the scene's colour and test its depth.
        const vk::MemoryBarrier2 written{
            .srcStageMask = Output | DepthTests,
            .srcAccessMask = ColorWrite | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            .dstStageMask = Output | DepthTests,
            .dstAccessMask = ColorAccess | DepthAccess,
        };
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written}, dl);
    }
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
    const vk::Viewport viewport = Upscaler::SceneViewport(extent);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    const std::array sets{Textures::Set(), Shadows::Set(), g_set};
    const GlassPush push{
        .draws = address + Scene::DrawsOffset,
        .materials = address + Scene::GlassMaterialsOffset,
        .light = address + Scene::LightOffset,
    };
    const u64 commands = offset + Scene::CommandsOffset +
                         u64{Scene::FirstCommand(header, List)} * sizeof(Scene::Command);
    for (u32 pass = 0; pass < g_passes; pass++) {
        const Pipeline& pipeline = g_pipelines[pass];
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {}, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
        cmd.drawIndexedIndirect(buffer, commands, count, sizeof(Scene::Command), dl);
    }
    cmd.endRendering(dl);
}

} // namespace Gr2Port::Glass
