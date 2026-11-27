// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <optional>
#include <utility>

#include "common/logging/log.h"
#include "common/thread.h"
#include "graphics/gr2_fsr3/vk_trampolines.h"
#include "graphics/gr2port/frame_generation.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/shaders/frame_generation_depth_comp.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/ui.h"
#include "graphics/gr2port/upscaler.h"
#include "graphics/gr2port/vblank.h"

// <vulkan/vulkan.h> must first be included through vk_common.h, which configures it.
#include <ffx_vk_fsr3_3_1_5_bridge.h>
#include <ffx_vk_portable.h>

namespace Gr2Port::FrameGeneration {

namespace {

using Clock = std::chrono::steady_clock;

constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;
constexpr vk::Format MotionFormat = vk::Format::eR16G16Sfloat;
// FSR 3.1.6's depth, which grows away from the camera.
constexpr vk::Format Fsr316DepthFormat = vk::Format::eR32Sfloat;
constexpr u32 GroupPixels = 8;
// How far below twice the game's frame rate the screen's refresh rate may be, as a share of it.
constexpr double Tolerance = 0.01;
// The longest wait between a frame's two presents: half a frame at 20 fps.
constexpr Clock::duration MaxHalfFrame = std::chrono::milliseconds{25};
// The subgroup operations of the frame interpolation's and the optical flow's shaders.
constexpr vk::SubgroupFeatureFlags SubgroupOperations = vk::SubgroupFeatureFlagBits::eBasic |
                                                        vk::SubgroupFeatureFlagBits::eArithmetic |
                                                        vk::SubgroupFeatureFlagBits::eQuad;

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
// Frame generation is chosen and the GPU runs it.
bool g_runs = false;
// FSR 3.1.6's frame generation runs where the device takes derivatives in compute shaders and its
// context is made, else FSR 3.1.4's.
bool g_fsr316 = false;
FfxVkFsr3_3_1_6FrameGenerationContext* g_context_316 = nullptr;
// FSR 3.1.6 keeps a frame's image views until the GPU is done with it: the frame ids it keeps,
// each with the renderer frame that recorded it.
std::deque<std::pair<u64, u64>> g_kept;
// The pass that writes FSR 3.1.6's depth from the scene's.
vk::Sampler g_sampler;
vk::DescriptorSetLayout g_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Pipeline g_depth_pass;
Image g_depth;
FfxVkPortableFrameGenerationContext* g_context = nullptr;
// What FSR 3.1.4's context was made with, which each frame is checked against.
FfxVkPortableFrameGenerationCreateInfo g_create{};
vk::Extent2D g_render;
vk::Extent2D g_picture;
// The back buffer as the composite left it, without the screen drawing.
Image g_hudless;
// The generated picture.
Image g_output;
u32 g_frame_rate = 30;
u64 g_frame_id = 0;
// The next frame starts the history again.
bool g_reset = true;
// This frame's world was copied.
bool g_copied = false;
// Whether the last frame took a generated present, as the log last said.
std::optional<bool> g_logged_presents;
u64 g_logged_issues = 0;
bool g_failed_logged = false;
// The game's frame time, from the generated presents.
Clock::duration g_interval{};
Clock::time_point g_last_present{};

FfxVkPortableImage PortableImage(vk::Image image, vk::Format format, vk::Extent2D extent,
                                 vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                                 FfxVkPortableResourceState state) {
    return {
        .structSize = sizeof(FfxVkPortableImage),
        .image = static_cast<VkImage>(image),
        .format = static_cast<VkFormat>(format),
        .extent = {extent.width, extent.height},
        .mipCount = 1,
        .arrayLayers = 1,
        .usage = static_cast<VkImageUsageFlags>(usage),
        .aspect = static_cast<VkImageAspectFlags>(aspect),
        .state = state,
    };
}

// With the GPU idle: frees the context and the images.
void Free() {
    if (g_context_316 != nullptr) {
        ffxVkFsr3_3_1_6FrameGenerationContextDestroy(g_context_316);
        g_context_316 = nullptr;
    }
    g_kept.clear();
    if (g_context != nullptr) {
        ffxVkPortableFrameGenerationContextDestroy(g_context);
        g_context = nullptr;
    }
    g_memory->Destroy(g_depth);
    g_memory->Destroy(g_hudless);
    g_memory->Destroy(g_output);
}

// Makes FSR 3.1.6's context and its depth for a scene of render and a picture of picture.
bool MakeFsr316(vk::Extent2D render, vk::Extent2D picture) {
    const FfxVkFsr3_3_1_6FrameGenerationCreateInfo info{
        .physicalDevice = static_cast<VkPhysicalDevice>(g_device->gpu),
        .device = static_cast<VkDevice>(g_device->device),
        .maxRenderWidth = render.width,
        .maxRenderHeight = render.height,
        .displayWidth = picture.width,
        .displayHeight = picture.height,
        .colorFormat = static_cast<VkFormat>(Ui::TargetFormat),
    };
    if (const FfxVkFsr3_3_1_6FrameGenerationResult result =
            ffxVkFsr3_3_1_6FrameGenerationContextCreate(&info, &g_context_316);
        result != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        LOG_WARNING(Render_Vulkan,
                    "GR2port frame generation: the FSR 3.1.6 context was not made ({}); FSR "
                    "3.1.4 runs",
                    static_cast<s32>(result));
        return false;
    }
    g_depth =
        g_memory->CreateImage(Fsr316DepthFormat, render,
                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                              vk::ImageAspectFlagBits::eColor);
    return true;
}

// Makes FSR 3.1.4's context for a scene of render and a picture of picture.
bool MakeFsr314(vk::Extent2D render, vk::Extent2D picture) {
    const FfxVkPortableDeviceInfo device_info{
        .structSize = sizeof(FfxVkPortableDeviceInfo),
        .instance = static_cast<VkInstance>(g_device->instance),
        .physicalDevice = static_cast<VkPhysicalDevice>(g_device->gpu),
        .device = static_cast<VkDevice>(g_device->device),
        .getDeviceProcAddr = Gr2Fsr3::DeviceProcAddr(),
        .queue = static_cast<VkQueue>(g_device->queue),
        .queueFamilyIndex = g_device->queue_family,
        .synchronization2Enabled = VK_TRUE,
        .shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE,
    };
    // The scene's depth is reversed: cleared to 0, nearer is greater. The back buffer holds the
    // sRGB of the picture.
    g_create = {
        .structSize = sizeof(FfxVkPortableFrameGenerationCreateInfo),
        .flags = FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED,
        .maxRenderSize = {render.width, render.height},
        .displaySize = {picture.width, picture.height},
        .interpolationSourceFormat = static_cast<VkFormat>(Ui::TargetFormat),
        .outputFormat = static_cast<VkFormat>(Ui::TargetFormat),
    };
    const FfxVkPortableResult result =
        ffxVkPortableFrameGenerationContextCreate(&device_info, &g_create, &g_context);
    if (result != FFX_VK_PORTABLE_OK) {
        g_context = nullptr;
        LOG_ERROR(Render_Vulkan,
                  "GR2port frame generation: the FSR 3.1.4 context was not made ({})",
                  static_cast<s32>(result));
        return false;
    }
    return true;
}

// With the GPU idle: makes a context and the images for a scene of render and a picture of
// picture. Every pipeline is made with the context.
bool Make(vk::Extent2D render, vk::Extent2D picture) {
    Free();
    const auto start = Clock::now();
    if (g_fsr316 && !MakeFsr316(render, picture)) {
        g_fsr316 = false;
    }
    if (!g_fsr316 && !MakeFsr314(render, picture)) {
        return false;
    }
    g_hudless = g_memory->CreateImage(Ui::TargetFormat, picture,
                                      vk::ImageUsageFlagBits::eSampled |
                                          vk::ImageUsageFlagBits::eTransferSrc |
                                          vk::ImageUsageFlagBits::eTransferDst,
                                      vk::ImageAspectFlagBits::eColor);
    g_output =
        g_memory->CreateImage(Ui::TargetFormat, picture,
                              vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
                              vk::ImageAspectFlagBits::eColor);
    if (!g_hudless.image || !g_output.image || (g_fsr316 && !g_depth.image)) {
        LOG_ERROR(Render_Vulkan, "GR2port frame generation: no images for a {}x{} picture",
                  picture.width, picture.height);
        Free();
        return false;
    }
    g_render = render;
    g_picture = picture;
    g_reset = true;
    g_copied = false;
    g_logged_issues = 0;
    g_failed_logged = false;
    LOG_INFO(Render_Vulkan, "GR2port frame generation: FSR {} {}x{} -> {}x{}, made in {} ms",
             g_fsr316 ? "3.1.6" : "3.1.4", render.width, render.height, picture.width,
             picture.height,
             std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    return true;
}

void LogFailureOnce(const char* what, s32 result) {
    if (!g_failed_logged) {
        g_failed_logged = true;
        LOG_ERROR(Render_Vulkan, "GR2port frame generation: {} failed ({})", what, result);
    }
}

// Makes the pass that writes FSR 3.1.6's depth from the scene's.
bool MakeDepthPass(const Device& device) {
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
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute,
         &g_sampler},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    g_layout = CreateSetLayout(device, bindings);
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {vk::DescriptorType::eCombinedImageSampler, 1},
        {vk::DescriptorType::eStorageImage, 1},
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
    g_depth_pass = CreateComputePipeline(device, FRAME_GENERATION_DEPTH_COMP, 0, set_layouts);
    return static_cast<bool>(g_depth_pass.pipeline);
}

// FSR 3.1.6's frame generation from motion and picture, and from the depth its pass first writes
// from the scene's.
bool RecordFsr316(vk::CommandBuffer cmd, vk::Image motion, vk::Image picture,
                  const Upscaler::View& view, Upscaler::Offset jitter, bool reset) {
    const auto& dl = g_device->dispatch;
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto General = vk::ImageLayout::eGeneral;
    constexpr auto ReadOnly = vk::ImageLayout::eShaderReadOnlyOptimal;
    constexpr auto Sampled = vk::ImageUsageFlagBits::eSampled;
    constexpr auto Storage = vk::ImageUsageFlagBits::eStorage;
    constexpr vk::ImageUsageFlags Transfer =
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    // The last frame's interpolation read the depth.
    ImageBarrier(*g_device, cmd, g_depth.image, vk::ImageLayout::eUndefined, General, Compute,
                 vk::AccessFlagBits2::eNone, Compute, vk::AccessFlagBits2::eShaderStorageWrite);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_depth_pass.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_depth_pass.layout, 0, g_set, {}, dl);
    cmd.dispatch((g_render.width + GroupPixels - 1) / GroupPixels,
                 (g_render.height + GroupPixels - 1) / GroupPixels, 1, dl);
    ImageBarrier(*g_device, cmd, g_depth.image, General, ReadOnly, Compute,
                 vk::AccessFlagBits2::eShaderStorageWrite, Compute,
                 vk::AccessFlagBits2::eShaderSampledRead);
    const auto image = [](vk::Image image, vk::Format format, vk::Extent2D extent,
                          vk::ImageLayout layout, vk::ImageUsageFlags usage) {
        return FfxVkFsr3_3_1_6FrameGenerationImage{
            .image = static_cast<VkImage>(image),
            .format = static_cast<VkFormat>(format),
            .width = extent.width,
            .height = extent.height,
            .layout = static_cast<VkImageLayout>(layout),
            .usage = static_cast<VkImageUsageFlags>(usage),
        };
    };
    const FfxVkFsr3_3_1_6FrameGenerationImage color =
        image(picture, Ui::TargetFormat, g_picture, ReadOnly,
              vk::ImageUsageFlagBits::eColorAttachment | Sampled | Transfer);
    const FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{
        .commandBuffer = static_cast<VkCommandBuffer>(cmd),
        .color = color,
        .hudlessColor =
            image(g_hudless.image, Ui::TargetFormat, g_picture, ReadOnly, Sampled | Transfer),
        .depth = image(g_depth.image, Fsr316DepthFormat, g_render, ReadOnly, Sampled | Storage),
        .motionVectors = image(motion, MotionFormat, g_render, General, Sampled | Storage),
        .renderWidth = g_render.width,
        .renderHeight = g_render.height,
        .jitterOffsetX = jitter.x,
        .jitterOffsetY = jitter.y,
        // The velocity is clip-space motion: half the size in pixels per unit, y up.
        .motionVectorScaleX = 0.5f * static_cast<float>(g_render.width),
        .motionVectorScaleY = -0.5f * static_cast<float>(g_render.height),
        .frameTimeMilliseconds = view.frame_ms,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .cameraNear = view.near_plane,
        .cameraFar = view.far_plane,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = view.fov,
        // Frame interpolation 3.1.6 does not read the camera's place and axes.
        .frameId = g_frame_id,
        .reset = reset ? VK_TRUE : VK_FALSE,
    };
    // Without a distortion field, over the whole picture.
    const FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{
        .commandBuffer = static_cast<VkCommandBuffer>(cmd),
        .color = color,
        .output = image(g_output.image, Ui::TargetFormat, g_picture, General, Sampled | Storage),
        .displayWidth = g_picture.width,
        .displayHeight = g_picture.height,
        .frameTimeMilliseconds = view.frame_ms,
        .cameraNear = view.near_plane,
        .cameraFar = view.far_plane,
        .viewSpaceToMeters = 1.0f,
        .cameraVerticalFovRadians = view.fov,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB,
        .frameId = g_frame_id,
        .reset = reset ? VK_TRUE : VK_FALSE,
    };
    bool made = false;
    if (const FfxVkFsr3_3_1_6FrameGenerationResult prepared =
            ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(g_context_316, &prepare);
        prepared != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        LogFailureOnce("the prepare", static_cast<s32>(prepared));
    } else if (const FfxVkFsr3_3_1_6FrameGenerationResult dispatched =
                   ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(g_context_316, &dispatch);
               dispatched != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) {
        LogFailureOnce("the dispatch", static_cast<s32>(dispatched));
    } else {
        made = true;
    }
    return made;
}

// FSR 3.1.4's frame generation from depth, motion and picture.
bool RecordFsr314(vk::CommandBuffer cmd, vk::Image depth, vk::Image motion, vk::Image picture,
                  const Upscaler::View& view, Upscaler::Offset jitter, bool reset) {
    constexpr auto Depth = vk::ImageAspectFlagBits::eDepth;
    constexpr auto Color = vk::ImageAspectFlagBits::eColor;
    constexpr auto Sampled = vk::ImageUsageFlagBits::eSampled;
    const FfxVkPortableImage hudless = PortableImage(
        g_hudless.image, Ui::TargetFormat, g_picture,
        Sampled | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        Color, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    const FfxVkPortableFrameGenerationPrepareInfo prepare{
        .structSize = sizeof(FfxVkPortableFrameGenerationPrepareInfo),
        .commandBuffer = static_cast<VkCommandBuffer>(cmd),
        .depth = PortableImage(depth, DepthFormat, g_render, Sampled, Depth,
                               FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ),
        .motionVectors = PortableImage(motion, MotionFormat, g_render,
                                       Sampled | vk::ImageUsageFlagBits::eStorage, Color,
                                       FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ),
        .renderSize = {g_render.width, g_render.height},
        .jitterOffset = {jitter.x, jitter.y},
        // The velocity is clip-space motion: half the size in pixels per unit, y up.
        .motionVectorScale = {0.5f * static_cast<float>(g_render.width),
                              -0.5f * static_cast<float>(g_render.height)},
        .frameTimeMilliseconds = view.frame_ms,
        .cameraNear = view.near_plane,
        .cameraFar = view.far_plane,
        .cameraVerticalFovRadians = view.fov,
        .viewSpaceToMeters = 1.0f,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB,
        .reset = reset ? VK_TRUE : VK_FALSE,
        .frameId = g_frame_id,
    };
    const FfxVkPortableFrameGenerationDispatchInfo dispatch{
        .structSize = sizeof(FfxVkPortableFrameGenerationDispatchInfo),
        .commandBuffer = static_cast<VkCommandBuffer>(cmd),
        .currentColor = PortableImage(picture, Ui::TargetFormat, g_picture,
                                      vk::ImageUsageFlagBits::eColorAttachment | Sampled |
                                          vk::ImageUsageFlagBits::eTransferSrc |
                                          vk::ImageUsageFlagBits::eTransferDst,
                                      Color, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ),
        .hudlessColor = hudless,
        // Absent images still carry their size.
        .distortionField = {.structSize = sizeof(FfxVkPortableImage)},
        .output = PortableImage(g_output.image, Ui::TargetFormat, g_picture,
                                Sampled | vk::ImageUsageFlagBits::eStorage, Color,
                                FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS),
        .displaySize = {g_picture.width, g_picture.height},
        .interpolationRect = {0, 0, g_picture.width, g_picture.height},
        .frameTimeMilliseconds = view.frame_ms,
        .cameraNear = view.near_plane,
        .cameraFar = view.far_plane,
        .cameraVerticalFovRadians = view.fov,
        .viewSpaceToMeters = 1.0f,
        .minLuminance = 0.0f,
        .maxLuminance = 1.0f,
        .transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB,
        .reset = reset ? VK_TRUE : VK_FALSE,
        .frameId = g_frame_id,
    };
    bool made = false;
    if (const u64 issues = ffxVkPortableValidateFrameGenerationPrepareInfo(&g_create, &prepare) |
                           ffxVkPortableValidateFrameGenerationDispatchInfo(&g_create, &dispatch);
        issues != FFX_VK_PORTABLE_VALIDATION_NONE) {
        if (issues != g_logged_issues) {
            g_logged_issues = issues;
            LOG_ERROR(Render_Vulkan,
                      "GR2port frame generation: FSR 3.1.4 rejects the frame ({:#x})", issues);
        }
    } else if (const FfxVkPortableResult prepared =
                   ffxVkPortableFrameGenerationContextPrepare(g_context, &prepare, &hudless);
               prepared != FFX_VK_PORTABLE_OK) {
        LogFailureOnce("the prepare", static_cast<s32>(prepared));
    } else if (const FfxVkPortableResult dispatched =
                   ffxVkPortableFrameGenerationContextRecordDispatch(g_context, &dispatch);
               dispatched != FFX_VK_PORTABLE_OK) {
        LogFailureOnce("the dispatch", static_cast<s32>(dispatched));
    } else {
        made = true;
    }
    return made;
}

} // namespace

void Start(const Device& device, Memory& memory, vk::Extent2D render, vk::Extent2D picture) {
    g_device = &device;
    g_memory = &memory;
    if (!Settings::Get().graphics.frame_generation) {
        return;
    }
    const vk::PhysicalDeviceFeatures features = device.gpu.getFeatures(device.dispatch);
    const auto properties =
        device.gpu
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties>(
                device.dispatch);
    const auto& vk11 = properties.get<vk::PhysicalDeviceVulkan11Properties>();
    // The FidelityFX calls go through the dispatcher, which Upscaler::Start sets only with an
    // upscaler chosen.
    Gr2Fsr3::SetDispatcher(device.dispatch);
    Gr2Fsr3::SetPipelineCache(static_cast<VkPipelineCache>(device.pipeline_cache));
    FfxVkPortableDeviceCapabilities caps{.structSize = sizeof(caps)};
    if (ffxVkPortableQueryDeviceCapabilities(static_cast<VkPhysicalDevice>(device.gpu), &caps) !=
            FFX_VK_PORTABLE_OK ||
        caps.fsr3FrameGenerationPrerequisites != VK_TRUE ||
        features.shaderStorageImageWriteWithoutFormat == VK_FALSE ||
        features.shaderStorageImageReadWithoutFormat == VK_FALSE ||
        !device.storage_extended_formats ||
        !(vk11.subgroupSupportedStages & vk::ShaderStageFlagBits::eCompute) ||
        (vk11.subgroupSupportedOperations & SubgroupOperations) != SubgroupOperations) {
        LOG_WARNING(Render_Vulkan, "GR2port frame generation: the GPU cannot run FSR 3.1's frame "
                                   "generation; it stays off");
        return;
    }
    g_fsr316 = Upscaler::ComputeDerivatives();
    if (!g_fsr316) {
        LOG_INFO(Render_Vulkan, "GR2port frame generation: the device takes no derivatives in "
                                "compute shaders, which FSR 3.1.6 needs; FSR 3.1.4 runs");
    } else if (!MakeDepthPass(device)) {
        g_fsr316 = false;
        LOG_ERROR(Render_Vulkan,
                  "GR2port frame generation: FSR 3.1.6's depth pass was not made; FSR 3.1.4 runs");
    }
    g_runs = true;
    g_frame_rate = std::max(Settings::Get().game.frame_rate, 1u);
    g_interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{1}) / g_frame_rate;
    // Made before the game starts for the picture the game will name, so the pipelines go into the
    // pipeline cache; the scene's first resize keeps the context.
    Make(render, picture);
}

bool Made() {
    return g_context_316 != nullptr || g_context != nullptr;
}

void Resize(vk::Extent2D render, vk::Extent2D picture, vk::ImageView depth) {
    if (!g_runs) {
        return;
    }
    if (!Made() || render != g_render || picture != g_picture) {
        if (!Make(render, picture)) {
            return;
        }
    }
    g_reset = true;
    g_copied = false;
    Textures::SetExternal(Scene::GeneratedHandle, g_output.view);
    if (g_context_316 == nullptr) {
        return;
    }
    // The scene's depth image is made again with each resize.
    const vk::DescriptorImageInfo scene_info{
        .imageView = depth, .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal};
    const vk::DescriptorImageInfo depth_info{.imageView = g_depth.view,
                                             .imageLayout = vk::ImageLayout::eGeneral};
    const std::array<vk::WriteDescriptorSet, 2> writes{{
        {
            .dstSet = g_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &scene_info,
        },
        {
            .dstSet = g_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .pImageInfo = &depth_info,
        },
    }};
    g_device->device.updateDescriptorSets(writes, {}, g_device->dispatch);
}

bool Presents(bool hdr) {
    const double refresh = Vblank::RefreshRate();
    const bool presents = Made() && !hdr && refresh >= 2.0 * g_frame_rate * (1.0 - Tolerance);
    if (Made() && g_logged_presents != presents) {
        g_logged_presents = presents;
        if (presents) {
            LOG_INFO(Render_Vulkan,
                     "GR2port frame generation: on, a generated frame before each of the game's "
                     "{} fps at {:.2f} Hz",
                     g_frame_rate, refresh);
        } else if (hdr) {
            LOG_INFO(Render_Vulkan, "GR2port frame generation: paused, the window is HDR");
        } else {
            LOG_INFO(Render_Vulkan,
                     "GR2port frame generation: paused, the screen refreshes at {:.2f} Hz, less "
                     "than twice the game's {} fps",
                     refresh, g_frame_rate);
        }
    }
    if (!presents) {
        g_reset = true;
    }
    return presents;
}

void CopyHudless(vk::CommandBuffer cmd, vk::Image picture, vk::Extent2D extent) {
    g_copied = false;
    if (!Made() || extent != g_picture) {
        return;
    }
    const auto& dl = g_device->dispatch;
    constexpr auto Copy = vk::PipelineStageFlagBits2::eCopy;
    constexpr auto Color = vk::ImageAspectFlagBits::eColor;
    ImageBarrier(*g_device, cmd, picture, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eTransferSrcOptimal,
                 vk::PipelineStageFlagBits2::eColorAttachmentOutput |
                     vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eColorAttachmentWrite, Copy,
                 vk::AccessFlagBits2::eTransferRead);
    // The last frame's interpolation read the copy.
    ImageBarrier(*g_device, cmd, g_hudless.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eNone, Copy, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy region{
        .srcSubresource = {Color, 0, 0, 1},
        .dstSubresource = {Color, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    cmd.copyImage(picture, vk::ImageLayout::eTransferSrcOptimal, g_hudless.image,
                  vk::ImageLayout::eTransferDstOptimal, region, dl);
    // The screen drawing draws into the back buffer next.
    ImageBarrier(*g_device, cmd, picture, vk::ImageLayout::eTransferSrcOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Copy, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eShaderSampledRead);
    ImageBarrier(*g_device, cmd, g_hudless.image, vk::ImageLayout::eTransferDstOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Copy, vk::AccessFlagBits2::eTransferWrite,
                 vk::PipelineStageFlagBits2::eComputeShader,
                 vk::AccessFlagBits2::eShaderSampledRead);
    g_copied = true;
}

bool Record(vk::CommandBuffer cmd, u64 frame, u64 completed, vk::Image depth, vk::Image motion,
            vk::Image picture, vk::Extent2D picture_extent) {
    // FSR 3.1.6 lets go of the image views of the frames the GPU is done with.
    std::optional<u64> retired;
    while (!g_kept.empty() && g_kept.front().second <= completed) {
        retired = g_kept.front().first;
        g_kept.pop_front();
    }
    if (retired) {
        ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(g_context_316, *retired);
    }
    if (!Made() || !g_copied || picture_extent != g_picture) {
        g_copied = false;
        g_reset = true;
        return false;
    }
    g_copied = false;
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto Fragment = vk::PipelineStageFlagBits2::eFragmentShader;
    constexpr auto Read = vk::AccessFlagBits2::eShaderSampledRead;
    constexpr auto Depth = vk::ImageAspectFlagBits::eDepth;
    const Upscaler::View view = Upscaler::FrameView();
    const Upscaler::Offset jitter = Upscaler::Jitter();
    const bool reset = g_reset || view.cut;
    // The passes read the depth as a compute shader's sampled image and the back buffer after the
    // screen drawing; the last frame's present read the output.
    ImageBarrier(*g_device, cmd, depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                     vk::PipelineStageFlagBits2::eLateFragmentTests | Fragment,
                 vk::AccessFlagBits2::eNone, Compute, Read, Depth);
    ImageBarrier(*g_device, cmd, picture, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                 vk::AccessFlagBits2::eColorAttachmentWrite, Compute, Read);
    ImageBarrier(*g_device, cmd, g_output.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eGeneral, Fragment, vk::AccessFlagBits2::eNone, Compute,
                 vk::AccessFlagBits2::eShaderStorageWrite);
    const bool made = g_context_316 != nullptr
                          ? RecordFsr316(cmd, motion, picture, view, jitter, reset)
                          : RecordFsr314(cmd, depth, motion, picture, view, jitter, reset);
    if (g_context_316 != nullptr) {
        g_kept.emplace_back(g_frame_id, frame);
    }
    g_frame_id++;
    ImageBarrier(*g_device, cmd, depth, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eDepthStencilReadOnlyOptimal, Compute, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eEarlyFragmentTests | Fragment,
                 vk::AccessFlagBits2::eDepthStencilAttachmentRead | Read, Depth);
    // The next frame's screen drawing writes the back buffer after these reads.
    ImageBarrier(*g_device, cmd, picture, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Compute, vk::AccessFlagBits2::eNone,
                 Fragment, Read);
    ImageBarrier(*g_device, cmd, g_output.image, vk::ImageLayout::eGeneral,
                 vk::ImageLayout::eShaderReadOnlyOptimal, Compute,
                 vk::AccessFlagBits2::eShaderStorageWrite, Fragment, Read);
    g_reset = !made;
    return made && !reset;
}

void WaitHalfFrame(Clock::time_point presented) {
    // A sample counts at most twice or half the frame time so far, which a pause or a hitch then
    // moves only a little.
    if (g_last_present != Clock::time_point{}) {
        const Clock::duration frame =
            std::chrono::duration_cast<Clock::duration>(std::chrono::seconds{1}) / g_frame_rate;
        const Clock::duration sample =
            std::clamp(presented - g_last_present, g_interval / 2, g_interval * 2);
        g_interval = std::max(g_interval + (sample - g_interval) / 8, frame);
    }
    g_last_present = presented;
    const Clock::duration wait = presented + std::min(g_interval / 2, MaxHalfFrame) - Clock::now();
    if (wait > Clock::duration::zero()) {
        Common::AccurateSleep(std::chrono::duration_cast<std::chrono::nanoseconds>(wait), nullptr,
                              false);
    }
}

} // namespace Gr2Port::FrameGeneration
