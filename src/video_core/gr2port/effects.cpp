// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>

#include "common/logging/log.h"
#include "video_core/gr2port/effects.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/particles.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/self_translucent.h"
#include "video_core/gr2port/shaders/effect_frag.h"
#include "video_core/gr2port/shaders/effect_skinned_vert.h"
#include "video_core/gr2port/shaders/effect_unskinned_vert.h"
#include "video_core/gr2port/textures.h"

namespace Gr2Port::Effects {

namespace {

struct DrawPush {
    u64 draws;
    u64 lights;
};

struct BlendMode {
    vk::BlendFactor source_color;
    vk::BlendFactor target_color;
    vk::BlendFactor source_alpha;
    vk::BlendFactor target_alpha;
    vk::BlendOp color;
    vk::BlendOp alpha;
};

using Factor = vk::BlendFactor;
using Op = vk::BlendOp;

// The game's blend states, by an item's blend mode.
constexpr std::array<BlendMode, 8> BlendModes{{
    {Factor::eOne, Factor::eOneMinusSrcAlpha, Factor::eZero, Factor::eOne, Op::eAdd, Op::eAdd},
    {Factor::eDstColor, Factor::eOne, Factor::eZero, Factor::eOne, Op::eAdd, Op::eAdd},
    {Factor::eOne, Factor::eOne, Factor::eZero, Factor::eOne, Op::eReverseSubtract, Op::eAdd},
    {Factor::eOne, Factor::eOne, Factor::eZero, Factor::eOne, Op::eAdd, Op::eAdd},
    {Factor::eOne, Factor::eOneMinusSrcAlpha, Factor::eOne, Factor::eOneMinusSrcAlpha, Op::eAdd,
     Op::eReverseSubtract},
    {Factor::eDstColor, Factor::eOne, Factor::eOne, Factor::eOneMinusSrcAlpha, Op::eAdd,
     Op::eReverseSubtract},
    {Factor::eOne, Factor::eOne, Factor::eOne, Factor::eOneMinusSrcAlpha, Op::eReverseSubtract,
     Op::eReverseSubtract},
    {Factor::eOneMinusDstColor, Factor::eOneMinusSrcAlpha, Factor::eZero, Factor::eOne, Op::eAdd,
     Op::eAdd},
}};

struct DepthMode {
    vk::CullModeFlagBits cull;
    bool test;
    bool write;
};

// The game's depth states, by an item's depth mode.
constexpr std::array<DepthMode, 8> DepthModes{{
    {vk::CullModeFlagBits::eNone, false, false},
    {vk::CullModeFlagBits::eBack, false, false},
    {vk::CullModeFlagBits::eNone, true, false},
    {vk::CullModeFlagBits::eBack, true, false},
    {vk::CullModeFlagBits::eNone, false, false},
    {vk::CullModeFlagBits::eBack, false, false},
    {vk::CullModeFlagBits::eNone, true, true},
    {vk::CullModeFlagBits::eBack, true, true},
}};

// The game's lists: SelfTranslucent's rendering draws the first two, the others go to the scene.
constexpr u32 HiddenLists = 2;
constexpr u32 ListTransparentBg = 4;
constexpr u32 ListTransparentFg = 5;
constexpr u32 ListTopMost = 6;

// By whether the mesh is skinned, then by blend mode.
using Pipelines = std::array<std::array<Pipeline, BlendModes.size()>, 2>;

const Device* g_device = nullptr;
vk::Format g_scene_format;
vk::Format g_depth_format;
std::atomic<bool> g_ready{false};
std::thread g_builder;
Pipelines g_hidden;
Pipelines g_scene;
vk::Sampler g_depth_sampler;
vk::Sampler g_background_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;

vk::PipelineColorBlendAttachmentState BlendState(const BlendMode& mode) {
    return {
        .blendEnable = true,
        .srcColorBlendFactor = mode.source_color,
        .dstColorBlendFactor = mode.target_color,
        .colorBlendOp = mode.color,
        .srcAlphaBlendFactor = mode.source_alpha,
        .dstAlphaBlendFactor = mode.target_alpha,
        .alphaBlendOp = mode.alpha,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
}

// Builds the pipelines, whose programs run to thousands of instructions.
void Build() {
    const auto start = std::chrono::steady_clock::now();
    const std::array sets{Textures::SetLayout(), g_set_layout};
    const std::array<std::span<const u32>, 2> vertex{EFFECT_UNSKINNED_VERT, EFFECT_SKINNED_VERT};
    bool built = true;
    for (u32 skinned = 0; skinned < vertex.size(); skinned++) {
        for (u32 mode = 0; mode < BlendModes.size(); mode++) {
            const vk::PipelineColorBlendAttachmentState blend = BlendState(BlendModes[mode]);
            const GraphicsPipelineInfo hidden{
                .vertex = vertex[skinned],
                .fragment = EFFECT_FRAG,
                .color = SelfTranslucent::ColorFormat,
                .depth = SelfTranslucent::DepthFormat,
                .depth_test = true,
                .depth_compare = vk::CompareOp::eGreaterOrEqual,
                .stencil_attachment = true,
                .blend = &blend,
                .dynamic_depth_and_cull = true,
                .push_constant_bytes = sizeof(DrawPush),
                .set_layouts = sets,
            };
            // In the scene, the blend constant's alpha scales the target's alpha.
            vk::PipelineColorBlendAttachmentState scene_blend = blend;
            scene_blend.srcAlphaBlendFactor = Factor::eZero;
            scene_blend.dstAlphaBlendFactor = Factor::eConstantAlpha;
            scene_blend.alphaBlendOp = Op::eAdd;
            GraphicsPipelineInfo scene = hidden;
            scene.color = g_scene_format;
            scene.depth = g_depth_format;
            scene.stencil_attachment = false;
            scene.blend = &scene_blend;
            scene.dynamic_blend_constants = true;
            g_hidden[skinned][mode] = CreateGraphicsPipeline(*g_device, hidden);
            g_scene[skinned][mode] = CreateGraphicsPipeline(*g_device, scene);
            built = built && g_hidden[skinned][mode].pipeline && g_scene[skinned][mode].pipeline;
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    if (!built) {
        LOG_ERROR(Render_Vulkan, "GR2port effects: a pipeline failed, after {} ms", ms);
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2port effects: pipelines built in {} ms", ms);
    SavePipelineCache(*g_device);
    g_ready.store(true, std::memory_order_release);
}

// Draws the runs of the lists from first_list to before end_list, with the viewport set.
void DrawRuns(vk::CommandBuffer cmd, const Scene::EffectHeader& frame, const u8* packet,
              u64 address, vk::Buffer buffer, u64 offset, u32 first_list, u32 end_list,
              const Pipelines& pipelines, bool depth_writes) {
    const auto& dl = g_device->dispatch;
    std::array<Scene::EffectRun, Scene::MaxEffectDraws> runs;
    std::memcpy(runs.data(), packet + Scene::EffectRunsOffset,
                frame.runs * sizeof(Scene::EffectRun));
    // The point lights are not ported: the programs count none and never read the address.
    const DrawPush push{
        .draws = address + Scene::EffectDrawsOffset,
        .lights = address + Scene::ParticleLightOffset,
    };
    cmd.bindIndexBuffer(Meshes::IndexBuffer(), 0, vk::IndexType::eUint16, dl);
    for (u32 i = 0; i < frame.runs; i++) {
        const Scene::EffectRun& run = runs[i];
        if (run.list < first_list || run.list >= end_list) {
            continue;
        }
        const Pipeline& pipeline = pipelines[run.skinned][run.blend];
        const std::array sets{Textures::Set(), g_set};
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline, dl);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.layout, 0, sets, {}, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        const DepthMode& depth = DepthModes[run.depth_mode];
        cmd.setCullMode(depth.cull, dl);
        cmd.setDepthTestEnable(depth.test, dl);
        cmd.setDepthWriteEnable(depth.write && depth_writes, dl);
        cmd.drawIndexedIndirect(
            buffer, offset + Scene::EffectCommandsOffset + u64{run.first} * sizeof(Scene::Command),
            run.count, sizeof(Scene::Command), dl);
    }
}

bool HasList(const Scene::Header& header, u32 list) {
    return (header.effects.lists & (1u << list)) != 0 && g_ready.load(std::memory_order_acquire);
}

// Draws a list into the scene, whose depth the programs also read, so nothing writes it.
void RecordList(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
                vk::Buffer buffer, u64 offset, u32 list, vk::ImageView scene, vk::ImageView depth,
                vk::Extent2D extent) {
    const auto& dl = g_device->dispatch;
    const vk::MemoryBarrier2 colors{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &colors}, dl);
    const vk::RenderingAttachmentInfo color{
        .imageView = scene,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingAttachmentInfo depth_attachment{
        .imageView = depth,
        .imageLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eNone,
    };
    cmd.beginRendering({.renderArea = {{0, 0}, extent},
                        .layerCount = 1,
                        .colorAttachmentCount = 1,
                        .pColorAttachments = &color,
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
    // The top-most list clears the game's stencil bit 3, which the scene's alpha stands for.
    const std::array<float, 4> constants{0.0f, 0.0f, 0.0f, list == ListTopMost ? 0.0f : 1.0f};
    cmd.setBlendConstants(constants.data(), dl);
    DrawRuns(cmd, header.effects, packet, address, buffer, offset, list, list + 1, g_scene, false);
    cmd.endRendering(dl);
}

} // namespace

bool Start(const Device& device, vk::Format scene_format, vk::Format depth_format) {
    g_device = &device;
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

void Resize(vk::ImageView depth, vk::ImageView background) {
    const vk::DescriptorImageInfo depth_info{
        .imageView = depth,
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
}

void RecordHidden(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
                  vk::Buffer buffer, u64 offset) {
    const Scene::EffectHeader& frame = header.effects;
    if (frame.runs == 0 || !g_ready.load(std::memory_order_acquire) || !Particles::DepthReady()) {
        return;
    }
    DrawRuns(cmd, frame, packet, address, buffer, offset, 0, HiddenLists, g_hidden, true);
}

void RecordTransparentBg(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                         u64 address, vk::Buffer buffer, u64 offset, vk::ImageView scene,
                         const Image& depth, vk::Extent2D extent) {
    if (!HasList(header, ListTransparentBg)) {
        return;
    }
    const auto& dl = g_device->dispatch;
    constexpr auto Tests = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                           vk::PipelineStageFlagBits2::eLateFragmentTests;
    constexpr auto Attachment = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    constexpr auto ReadOnly = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr vk::AccessFlags2 DepthAccess = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    const vk::ImageMemoryBarrier2 to_read{
        .srcStageMask = Tests,
        .srcAccessMask = DepthAccess,
        .dstStageMask = Tests | Fragment,
        .dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                         vk::AccessFlagBits2::eShaderSampledRead,
        .oldLayout = Attachment,
        .newLayout = ReadOnly,
        .image = depth.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read}, dl);
    RecordList(cmd, header, packet, address, buffer, offset, ListTransparentBg, scene, depth.view,
               extent);
    const vk::ImageMemoryBarrier2 to_write{
        .srcStageMask = Tests | Fragment,
        .dstStageMask = Tests,
        .dstAccessMask = DepthAccess,
        .oldLayout = ReadOnly,
        .newLayout = Attachment,
        .image = depth.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_write}, dl);
}

void RecordTransparentFg(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                         u64 address, vk::Buffer buffer, u64 offset, vk::ImageView scene,
                         vk::ImageView depth, vk::Extent2D extent) {
    if (HasList(header, ListTransparentFg)) {
        RecordList(cmd, header, packet, address, buffer, offset, ListTransparentFg, scene, depth,
                   extent);
    }
}

void RecordTopMost(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                   u64 address, vk::Buffer buffer, u64 offset, vk::ImageView scene,
                   vk::ImageView depth, vk::Extent2D extent) {
    if (HasList(header, ListTopMost)) {
        RecordList(cmd, header, packet, address, buffer, offset, ListTopMost, scene, depth, extent);
    }
}

} // namespace Gr2Port::Effects
