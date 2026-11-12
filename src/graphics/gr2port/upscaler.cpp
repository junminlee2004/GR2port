// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "graphics/gr2_fsr3/dlss_streamline.h"
#include "graphics/gr2_fsr3/fsr4_assets.h"
#include "graphics/gr2_fsr3/fsr4_v07_bridge.h"
#include "graphics/gr2_fsr3/shaders/fsr4_inputs_comp.h"
#include "graphics/gr2_fsr3/vk_trampolines.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/upscaler.h"

// <vulkan/vulkan.h> must first be included through vk_common.h, which configures it.
#include <ffx_vk_portable.h>

namespace Gr2Port::Upscaler {

namespace {

using Clock = std::chrono::steady_clock;

// Each upscaler writes its picture in the toned colour's format, which compute shaders then
// encode.
constexpr vk::Format OutputFormat = vk::Format::eR16G16B16A16Sfloat;
// FSR 4 and DLSS read copies of the inputs in these formats.
constexpr vk::Format InputColorFormat = vk::Format::eR16G16B16A16Sfloat;
constexpr vk::Format InputMotionFormat = vk::Format::eR16G16Sfloat;
constexpr vk::Format InputDepthFormat = vk::Format::eR32Sfloat;
constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;
constexpr vk::Format MotionFormat = vk::Format::eR16G16Sfloat;
// A frame after a longer pause, or the first after a resize, restarts the upscaler's history.
constexpr auto ResetGap = std::chrono::milliseconds{100};
constexpr float MaxFrameMs = 200.0f;
constexpr float FirstFrameMs = 33.3f;
constexpr u32 InputGroupPixels = 8;
// DLSS's convolutional model, preset E, in every mode; NGX's default picks the transformer model.
constexpr Gr2Fsr3::DlssPreset DlssModel = Gr2Fsr3::DlssPreset::E;
// A camera that moves or turns further than this between frames, or changes its field of view by
// more, cuts: the game raises its own reset at the same distance.
constexpr float CutTranslation = 30.0f;
constexpr float CutRotationDegrees = 45.0f;
constexpr float CutFov = 0.35f;

// The view's projection terms as the upscalers take them.
struct Camera {
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float fov = 0.87f;
};

// The inputs pass's push constants, as fsr4_inputs.comp declares them.
struct InputsPush {
    std::array<u32, 2> size;
    u32 flags;
    float near_plane;
    float far_plane;
};

// An FSR 4 dispatch the GPU may still run, with the renderer's frame that recorded it.
struct Fsr4Frame {
    u64 id;
    u64 frame;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
// The running upscaler: the chosen one, or Off when it cannot run.
Settings::Upscaler g_upscaler = Settings::Upscaler::Off;
Settings::UpscaleQuality g_quality = Settings::UpscaleQuality::Quality;
// The output's size over the scene's. The capture reads it on the game's thread.
std::atomic<float> g_ratio{1.0f};
Image g_output;
vk::Extent2D g_render;
vk::Extent2D g_output_extent;
vk::Image g_depth;
vk::Image g_velocity;
vk::Image g_toned;
vk::Format g_toned_format = vk::Format::eUndefined;
s32 g_phase = 0;
s32 g_phase_count = 1;
Offset g_jitter;
Camera g_camera;
bool g_camera_seen = false;
float g_frame_ms = FirstFrameMs;
Clock::time_point g_last_frame{};
bool g_reset = true;
u64 g_frame_id = 0;
u64 g_renderer_frame = 0;
u32 g_frames_in_flight = 1;
bool g_failed_logged = false;

// FSR 3.1.
FfxVkPortableUpscaleContext* g_fsr3 = nullptr;
// What the context was made with, which each dispatch is checked against.
FfxVkPortableUpscaleCreateInfo g_fsr3_create{};
u64 g_fsr3_logged_issues = 0;

// FSR 4 and DLSS: the inputs pass and its copies of the inputs.
vk::DescriptorSetLayout g_inputs_layout;
vk::DescriptorPool g_inputs_pool;
vk::DescriptorSet g_inputs_set;
Pipeline g_inputs;
Image g_input_color;
Image g_input_motion;
Image g_input_depth;

// FSR 4: the features the device was made with, the context and its unretired dispatches.
vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR g_derivatives{};
std::vector<const char*> g_device_extensions;
Gr2Fsr3::Fsr4DeviceFeatures g_fsr4_features{};
Gr2Fsr4Context* g_fsr4 = nullptr;
std::deque<Fsr4Frame> g_fsr4_frames;

// DLSS: whether Streamline holds options for the sizes.
bool g_dlss_ready = false;

const char* Name(Settings::Upscaler upscaler) {
    switch (upscaler) {
    case Settings::Upscaler::Fsr3:
        return "FSR 3.1";
    case Settings::Upscaler::Fsr4:
        return "FSR 4";
    case Settings::Upscaler::Dlss:
        return "DLSS";
    default:
        return "off";
    }
}

bool UsesInputs() {
    return g_upscaler == Settings::Upscaler::Fsr4 || g_upscaler == Settings::Upscaler::Dlss;
}

// The radical inverse of index in base: the Halton sequence the jitter follows.
float Halton(s32 index, s32 base) {
    float fraction = 1.0f;
    float result = 0.0f;
    for (s32 i = index; i > 0; i /= base) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(i % base);
    }
    return result;
}

// The jitter sequence's length: 8 times the square of the output over render width, as FSR 3.1
// and the FSR 4 provider compute it, and at least 16 for DLSS.
s32 PhaseCount(vk::Extent2D render, vk::Extent2D output) {
    const float scale = static_cast<float>(output.width) / static_cast<float>(render.width);
    const s32 count = std::clamp(static_cast<s32>(std::ceil(8.0f * scale * scale)), 1, 256);
    return g_upscaler == Settings::Upscaler::Dlss ? std::max(count, 16) : count;
}

u32 Fsr4Preset(Settings::UpscaleQuality quality) {
    switch (quality) {
    case Settings::UpscaleQuality::Native:
        return GR2_FSR4_PRESET_NATIVE;
    case Settings::UpscaleQuality::Balanced:
        return GR2_FSR4_PRESET_BALANCED;
    case Settings::UpscaleQuality::Performance:
        return GR2_FSR4_PRESET_PERFORMANCE;
    case Settings::UpscaleQuality::UltraPerformance:
        return GR2_FSR4_PRESET_ULTRA_PERFORMANCE;
    default:
        return GR2_FSR4_PRESET_QUALITY;
    }
}

Gr2Fsr3::DlssMode DlssModeOf(Settings::UpscaleQuality quality) {
    switch (quality) {
    case Settings::UpscaleQuality::Native:
        return Gr2Fsr3::DlssMode::Dlaa;
    case Settings::UpscaleQuality::Balanced:
        return Gr2Fsr3::DlssMode::Balanced;
    case Settings::UpscaleQuality::Performance:
        return Gr2Fsr3::DlssMode::Performance;
    case Settings::UpscaleQuality::UltraPerformance:
        return Gr2Fsr3::DlssMode::UltraPerformance;
    default:
        return Gr2Fsr3::DlssMode::Quality;
    }
}

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

void ImageBarrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageAspectFlags aspect,
                  vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

void WriteInput(u32 binding, vk::DescriptorType type, vk::ImageView view, vk::ImageLayout layout) {
    if (!g_inputs_set || !view) {
        return;
    }
    const vk::DescriptorImageInfo info{.imageView = view, .imageLayout = layout};
    const vk::WriteDescriptorSet write{
        .dstSet = g_inputs_set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
}

// With the GPU idle: frees the running upscaler's context and the images.
void FreeContexts() {
    if (g_fsr3 != nullptr) {
        ffxVkPortableUpscaleContextDestroy(g_fsr3);
        g_fsr3 = nullptr;
    }
    if (g_fsr4 != nullptr) {
        Gr2Fsr4Destroy(g_fsr4);
        g_fsr4 = nullptr;
        g_fsr4_frames.clear();
    }
    if (g_dlss_ready) {
        Gr2Fsr3::DlssFree();
        g_dlss_ready = false;
    }
    for (Image* image : {&g_output, &g_input_color, &g_input_motion, &g_input_depth}) {
        g_memory->Destroy(*image);
    }
}

// The game's antialiasing runs from now on.
void TurnOff(std::string_view why) {
    LOG_ERROR(Render_Vulkan, "GR2port upscaler: {}; the game's antialiasing runs", why);
    FreeContexts();
    g_upscaler = Settings::Upscaler::Off;
    g_ratio = 1.0f;
}

bool CreateFsr3(vk::Extent2D render, vk::Extent2D output) {
    const FfxVkPortableDeviceInfo device_info{
        .structSize = sizeof(FfxVkPortableDeviceInfo),
        .instance = static_cast<VkInstance>(g_device->instance),
        .physicalDevice = static_cast<VkPhysicalDevice>(g_device->gpu),
        .device = static_cast<VkDevice>(g_device->device),
        .getDeviceProcAddr = g_device->dispatch.vkGetDeviceProcAddr,
        .queue = static_cast<VkQueue>(g_device->queue),
        .queueFamilyIndex = g_device->queue_family,
        .synchronization2Enabled = VK_TRUE,
        .shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE,
    };
    // The scene's depth is reversed: cleared to 0, nearer is greater.
    g_fsr3_create = {
        .structSize = sizeof(FfxVkPortableUpscaleCreateInfo),
        .flags = FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED,
        .maxRenderSize = {render.width, render.height},
        .maxOutputSize = {output.width, output.height},
    };
    const FfxVkPortableResult result =
        ffxVkPortableUpscaleContextCreate(&device_info, &g_fsr3_create, &g_fsr3);
    if (result != FFX_VK_PORTABLE_OK) {
        g_fsr3 = nullptr;
        TurnOff(fmt::format("the FSR 3.1 context was not made ({})", static_cast<s32>(result)));
        return false;
    }
    g_fsr3_logged_issues = 0;
    return true;
}

bool CreateInputs(vk::Extent2D render) {
    // DLSS clears one of the images it is given, which takes the transfer usage.
    constexpr auto Usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                           vk::ImageUsageFlagBits::eTransferDst;
    g_input_color =
        g_memory->CreateImage(InputColorFormat, render, Usage, vk::ImageAspectFlagBits::eColor);
    g_input_motion =
        g_memory->CreateImage(InputMotionFormat, render, Usage, vk::ImageAspectFlagBits::eColor);
    g_input_depth =
        g_memory->CreateImage(InputDepthFormat, render, Usage, vk::ImageAspectFlagBits::eColor);
    if (!g_input_color.image || !g_input_motion.image || !g_input_depth.image) {
        return false;
    }
    WriteInput(3, vk::DescriptorType::eStorageImage, g_input_color.view, vk::ImageLayout::eGeneral);
    WriteInput(4, vk::DescriptorType::eStorageImage, g_input_motion.view,
               vk::ImageLayout::eGeneral);
    WriteInput(5, vk::DescriptorType::eStorageImage, g_input_depth.view, vk::ImageLayout::eGeneral);
    return true;
}

bool CreateFsr4(vk::Extent2D render, vk::Extent2D output) {
    const auto properties =
        g_device->gpu
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties>(
                g_device->dispatch);
    const auto& vk11 = properties.get<vk::PhysicalDeviceVulkan11Properties>();
    const bool compute_subgroups =
        static_cast<bool>(vk11.subgroupSupportedStages & vk::ShaderStageFlagBits::eCompute);
    const Gr2Fsr3::Fsr4DeviceLimits limits{
        .features = g_fsr4_features,
        .subgroup_operations =
            compute_subgroups ? static_cast<u32>(vk11.subgroupSupportedOperations) : 0u,
        .max_storage_buffer_range = properties.get().properties.limits.maxStorageBufferRange,
        .max_allocation_size = vk11.maxMemoryAllocationSize,
    };
    const std::filesystem::path folder =
        Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "fsr4_v07";
    const Gr2Fsr3::Fsr4LoadResult loaded =
        Gr2Fsr3::LoadFsr4Assets(folder, Fsr4Preset(g_quality), output, limits);
    if (!loaded.assets) {
        TurnOff(fmt::format("FSR 4 has no usable files in {}: {}",
                            Common::FS::PathToUTF8String(folder), loaded.notice));
        return false;
    }
    const Gr2Fsr3::Fsr4Assets& assets = *loaded.assets;
    Gr2Fsr4CreateInfo info{
        .device = static_cast<VkDevice>(g_device->device),
        .physical_device = static_cast<VkPhysicalDevice>(g_device->gpu),
        .initializer = assets.initializer.data(),
        .initializer_size = assets.initializer.size(),
        .pre_weights = assets.pre_weights.data(),
        .pre_weights_size = assets.pre_weights.size(),
        .max_render_width = render.width,
        .max_render_height = render.height,
        .max_output_width = output.width,
        .max_output_height = output.height,
    };
    for (std::size_t pass = 0; pass < Gr2Fsr3::Fsr4ShaderCount; ++pass) {
        info.shaders[pass] = Gr2Fsr4Shader{
            .words = assets.shaders[pass].data(),
            .size_bytes = assets.shaders[pass].size() * sizeof(u32),
            .entry_point = assets.entry_points[pass].c_str(),
        };
    }
    const Gr2Fsr4Status status = Gr2Fsr4Create(&info, &g_fsr4);
    if (status.stage != GR2_FSR4_STAGE_NONE) {
        g_fsr4 = nullptr;
        TurnOff(fmt::format("the FSR 4 context was not made (stage {}, Vulkan {}, provider {})",
                            status.stage, status.vk_result, status.api_result));
        return false;
    }
    LOG_INFO(Render_Vulkan, "GR2port upscaler: FSR 4 files: {}", loaded.summary);
    return true;
}

bool CreateDlss(vk::Extent2D render, vk::Extent2D output) {
    const Gr2Fsr3::DlssMode mode = DlssModeOf(g_quality);
    std::string notice;
    const std::optional<Gr2Fsr3::DlssRange> range =
        Gr2Fsr3::DlssOptimalRange(mode, DlssModel, output, notice);
    if (!range) {
        TurnOff(notice);
        return false;
    }
    if (render.width < range->min.width || render.height < range->min.height ||
        render.width > range->max.width || render.height > range->max.height) {
        TurnOff(fmt::format("DLSS mode {} takes {}x{} to {}x{} for {}x{}, not {}x{}",
                            static_cast<u32>(mode), range->min.width, range->min.height,
                            range->max.width, range->max.height, output.width, output.height,
                            render.width, render.height));
        return false;
    }
    if (const std::string rejected = Gr2Fsr3::DlssSetOptions(mode, DlssModel, output);
        !rejected.empty()) {
        TurnOff(rejected);
        return false;
    }
    g_dlss_ready = true;
    LOG_INFO(Render_Vulkan, "GR2port upscaler: DLSS runs preset E, the convolutional model");
    return true;
}

// Copies the toned colour, the velocity and the depth into the images FSR 4 and DLSS read.
void RecordInputs(vk::CommandBuffer cmd, float near_plane, float far_plane) {
    const auto& dl = g_device->dispatch;
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    for (const Image* image : {&g_input_color, &g_input_motion, &g_input_depth}) {
        // The last frame's upscaler read the image.
        ImageBarrier(cmd, image->image, vk::ImageAspectFlagBits::eColor,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral, Compute,
                     vk::AccessFlagBits2::eNone, Compute, vk::AccessFlagBits2::eShaderStorageWrite);
    }
    const InputsPush push{
        .size = {g_render.width, g_render.height},
        .flags = 0,
        .near_plane = near_plane,
        .far_plane = far_plane,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_inputs.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_inputs.layout, 0, g_inputs_set, {},
                           dl);
    cmd.pushConstants(g_inputs.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push,
                      dl);
    cmd.dispatch((g_render.width + InputGroupPixels - 1) / InputGroupPixels,
                 (g_render.height + InputGroupPixels - 1) / InputGroupPixels, 1, dl);
    for (const Image* image : {&g_input_color, &g_input_motion, &g_input_depth}) {
        ImageBarrier(cmd, image->image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
                     vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderStorageWrite,
                     Compute, vk::AccessFlagBits2::eShaderRead);
    }
}

void LogFailureOnce(std::string_view what) {
    if (!g_failed_logged) {
        g_failed_logged = true;
        LOG_ERROR(Render_Vulkan, "GR2port upscaler: {}", what);
    }
}

// Takes the frame's projection terms and resets the history on a camera cut. The light's
// view_to_previous M maps the view to the last frame's clip space; with the inverse projection IV,
// R = M x P^-1 maps the view to the last frame's view while the projection holds.
void ReadCamera(const u8* packet) {
    std::array<float, 16> m;
    std::array<float, 4> iv;
    const u8* light = packet + Scene::LightOffset;
    std::memcpy(m.data(), light + offsetof(Scene::Light, view_to_previous), sizeof(m));
    std::memcpy(iv.data(), light + offsetof(Scene::Light, inverse_projection), sizeof(iv));
    const float near_plane = 1.0f / (iv[3] - iv[2]);
    const float far_plane = 1.0f / (iv[2] + iv[3]);
    const float fov = 2.0f * std::atan(iv[1]);
    if (!(near_plane > 0.0f) || !(far_plane > near_plane) || !std::isfinite(far_plane) ||
        !(fov > 0.0f) || !(fov < std::numbers::pi_v<float>)) {
        return;
    }
    const float r00 = m[0] * iv[0];
    const float r01 = m[1] * iv[1];
    const float r02 = -m[3];
    const float r10 = m[4] * iv[0];
    const float r11 = m[5] * iv[1];
    const float r12 = -m[7];
    const float r20 = m[8] * iv[0];
    const float r21 = m[9] * iv[1];
    const float r22 = -m[11];
    const float translation = std::hypot(m[12] * iv[0], m[13] * iv[1], -m[15]);
    // The sine from the antisymmetric part keeps small angles exact.
    const float sine = 0.5f * std::hypot(r21 - r12, r02 - r20, r10 - r01);
    const float cosine = 0.5f * (r00 + r11 + r22 - 1.0f);
    const float degrees = std::atan2(sine, cosine) * (180.0f / std::numbers::pi_v<float>);
    if (g_camera_seen && (translation > CutTranslation || degrees > CutRotationDegrees ||
                          std::abs(fov - g_camera.fov) > CutFov)) {
        g_reset = true;
    }
    g_camera = {.near_plane = near_plane, .far_plane = far_plane, .fov = fov};
    g_camera_seen = true;
}

void RecordFsr3(vk::CommandBuffer cmd, float camera_near, float camera_far, float fov) {
    constexpr auto Depth = vk::ImageAspectFlagBits::eDepth;
    const FfxVkPortableUpscaleDispatchInfo info{
        .structSize = sizeof(FfxVkPortableUpscaleDispatchInfo),
        .commandBuffer = static_cast<VkCommandBuffer>(cmd),
        .color = PortableImage(g_toned, g_toned_format, g_render, vk::ImageUsageFlagBits::eSampled,
                               vk::ImageAspectFlagBits::eColor,
                               FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ),
        .depth = PortableImage(g_depth, DepthFormat, g_render, vk::ImageUsageFlagBits::eSampled,
                               Depth, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ),
        .motionVectors = PortableImage(
            g_velocity, MotionFormat, g_render,
            vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
            vk::ImageAspectFlagBits::eColor, FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ),
        // Absent images still carry their size.
        .exposure = {.structSize = sizeof(FfxVkPortableImage)},
        .reactiveMask = {.structSize = sizeof(FfxVkPortableImage)},
        .transparencyAndCompositionMask = {.structSize = sizeof(FfxVkPortableImage)},
        .output = PortableImage(g_output.image, OutputFormat, g_output_extent,
                                vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
                                vk::ImageAspectFlagBits::eColor,
                                FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS),
        .jitterOffset = {g_jitter.x, g_jitter.y},
        // The velocity is clip-space motion: half the size in pixels per unit, y up.
        .motionVectorScale = {0.5f * static_cast<float>(g_render.width),
                              -0.5f * static_cast<float>(g_render.height)},
        .renderSize = {g_render.width, g_render.height},
        .outputSize = {g_output_extent.width, g_output_extent.height},
        .frameTimeMilliseconds = g_frame_ms,
        .preExposure = 1.0f,
        .cameraNear = camera_near,
        .cameraFar = camera_far,
        .cameraVerticalFovRadians = fov,
        .viewSpaceToMeters = 1.0f,
        .reset = g_reset ? VK_TRUE : VK_FALSE,
        .frameId = g_frame_id++,
    };
    if (const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&g_fsr3_create, &info);
        issues != FFX_VK_PORTABLE_VALIDATION_NONE) {
        if (issues != g_fsr3_logged_issues) {
            g_fsr3_logged_issues = issues;
            LOG_ERROR(Render_Vulkan, "GR2port upscaler: FSR 3.1 rejects the frame ({:#x})", issues);
        }
    } else if (const FfxVkPortableResult result =
                   ffxVkPortableUpscaleContextRecordDispatch(g_fsr3, &info);
               result != FFX_VK_PORTABLE_OK) {
        LogFailureOnce(fmt::format("FSR 3.1 dispatch failed ({})", static_cast<s32>(result)));
    }
}

void RecordFsr4(vk::CommandBuffer cmd) {
    // The dispatches of frames the GPU has finished go back to the provider.
    std::optional<u64> completed;
    while (!g_fsr4_frames.empty() &&
           g_fsr4_frames.front().frame + g_frames_in_flight <= g_renderer_frame) {
        completed = g_fsr4_frames.front().id;
        g_fsr4_frames.pop_front();
    }
    if (completed) {
        if (const s32 result = Gr2Fsr4Retire(g_fsr4, *completed); result != VK_SUCCESS) {
            LogFailureOnce(fmt::format("FSR 4 cannot release frame {} ({})", *completed, result));
        }
    }
    const auto image = [](const Image& source) {
        return Gr2Fsr4Image{
            .image = static_cast<VkImage>(source.image),
            .view = static_cast<VkImageView>(source.view),
            .width = source.extent.width,
            .height = source.extent.height,
        };
    };
    const Gr2Fsr4DispatchInfo info{
        .command_buffer = static_cast<VkCommandBuffer>(cmd),
        .frame_id = g_frame_id,
        .color = image(g_input_color),
        .depth = image(g_input_depth),
        .motion_vectors = image(g_input_motion),
        .output = image(g_output),
        .jitter_x = g_jitter.x,
        .jitter_y = g_jitter.y,
        .motion_vector_scale_x = 0.5f * static_cast<float>(g_render.width),
        .motion_vector_scale_y = -0.5f * static_cast<float>(g_render.height),
        .render_width = g_render.width,
        .render_height = g_render.height,
        .output_width = g_output_extent.width,
        .output_height = g_output_extent.height,
        .reset = g_reset ? 1u : 0u,
    };
    const Gr2Fsr4Status status = Gr2Fsr4Dispatch(g_fsr4, &info);
    if (status.stage != GR2_FSR4_STAGE_NONE) {
        LogFailureOnce(fmt::format("FSR 4 rejected the dispatch (stage {}, Vulkan {}, provider {})",
                                   status.stage, status.vk_result, status.api_result));
        return;
    }
    g_fsr4_frames.push_back({.id = g_frame_id++, .frame = g_renderer_frame});
}

void RecordDlss(vk::CommandBuffer cmd) {
    const auto image = [](const Image& source) {
        return Gr2Fsr3::Fsr4Image{.image = source.image, .view = source.view};
    };
    const Gr2Fsr3::DlssEvaluateInfo info{
        .images =
            {
                .color = image(g_input_color),
                .motion_vectors = image(g_input_motion),
                .depth = image(g_input_depth),
                .output = image(g_output),
            },
        .render_size = g_render,
        .output_size = g_output_extent,
        .jitter_x = g_jitter.x,
        .jitter_y = g_jitter.y,
        // Streamline takes the motion as a fraction of the screen: half per clip-space unit.
        .motion_vector_scale_x = 0.5f,
        .motion_vector_scale_y = -0.5f,
        .reset = g_reset,
    };
    if (const std::string failed = Gr2Fsr3::DlssEvaluate(cmd, info); !failed.empty()) {
        LogFailureOnce(failed);
    }
}

} // namespace

void ExtendInstance(std::vector<const char*>& extensions,
                    const vk::detail::DispatchLoaderDynamic& dispatch) {
    Gr2Fsr3::ExtendInstance(extensions, dispatch);
}

void ExtendDevice(vk::PhysicalDevice gpu, const vk::detail::DispatchLoaderDynamic& dispatch,
                  vk::DeviceCreateInfo& info, vk::PhysicalDeviceFeatures2& features,
                  vk::PhysicalDeviceVulkan11Features& vk11,
                  vk::PhysicalDeviceVulkan12Features& vk12,
                  vk::PhysicalDeviceVulkan13Features& vk13) {
    const Settings::Upscaler chosen = Settings::Get().graphics.upscaler;
    if (chosen == Settings::Upscaler::Off) {
        return;
    }
    const auto supported =
        gpu.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                         vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>(
            dispatch);
    const vk::PhysicalDeviceFeatures& core = supported.get().features;
    // FSR 3.1's accumulate shaders and FSR 4's write storage images declared without a format.
    features.features.shaderStorageImageWriteWithoutFormat =
        core.shaderStorageImageWriteWithoutFormat;
    if (chosen == Settings::Upscaler::Dlss) {
        // Streamline makes a private data slot on the device.
        vk13.privateData = supported.get<vk::PhysicalDeviceVulkan13Features>().privateData;
        Gr2Fsr3::ExtendDeviceForDlss(gpu, info, vk12, dispatch);
        return;
    }
    if (chosen != Settings::Upscaler::Fsr4) {
        return;
    }
    // What the FSR 4 v07 shaders can declare, as far as the GPU has it.
    const auto& supported11 = supported.get<vk::PhysicalDeviceVulkan11Features>();
    const auto& supported12 = supported.get<vk::PhysicalDeviceVulkan12Features>();
    features.features.shaderStorageImageReadWithoutFormat =
        core.shaderStorageImageReadWithoutFormat;
    features.features.shaderImageGatherExtended = core.shaderImageGatherExtended;
    features.features.shaderInt16 = core.shaderInt16;
    features.features.shaderFloat64 = core.shaderFloat64;
    vk11.storageBuffer16BitAccess = supported11.storageBuffer16BitAccess;
    vk11.uniformAndStorageBuffer16BitAccess = supported11.uniformAndStorageBuffer16BitAccess;
    vk12.shaderFloat16 = supported12.shaderFloat16;
    vk12.shaderInt8 = supported12.shaderInt8;
    vk12.storageBuffer8BitAccess = supported12.storageBuffer8BitAccess;
    vk12.uniformAndStorageBuffer8BitAccess = supported12.uniformAndStorageBuffer8BitAccess;
    vk13.shaderIntegerDotProduct =
        supported.get<vk::PhysicalDeviceVulkan13Features>().shaderIntegerDotProduct;
    const auto [result, extensions] = gpu.enumerateDeviceExtensionProperties(nullptr, dispatch);
    const bool has_derivatives =
        result == vk::Result::eSuccess &&
        std::ranges::any_of(extensions, [](const vk::ExtensionProperties& extension) {
            return std::string_view{extension.extensionName.data()} ==
                   VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
        });
    bool derivatives = false;
    if (has_derivatives) {
        const auto queried =
            gpu.getFeatures2<vk::PhysicalDeviceFeatures2,
                             vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>(dispatch)
                .get<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
        derivatives = queried.computeDerivativeGroupLinear == VK_TRUE;
        if (derivatives) {
            // The create info points at these until the device is made.
            g_derivatives = vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR{
                .pNext = features.pNext,
                .computeDerivativeGroupQuads = queried.computeDerivativeGroupQuads,
                .computeDerivativeGroupLinear = VK_TRUE,
            };
            features.pNext = &g_derivatives;
            g_device_extensions.assign(info.ppEnabledExtensionNames,
                                       info.ppEnabledExtensionNames + info.enabledExtensionCount);
            g_device_extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
            info.enabledExtensionCount = static_cast<u32>(g_device_extensions.size());
            info.ppEnabledExtensionNames = g_device_extensions.data();
        }
    }
    const vk::PhysicalDeviceFeatures& enabled = features.features;
    g_fsr4_features = Gr2Fsr3::Fsr4DeviceFeatures{
        .shader_float16 = vk12.shaderFloat16 == VK_TRUE,
        .shader_float64 = enabled.shaderFloat64 == VK_TRUE,
        .shader_int8 = vk12.shaderInt8 == VK_TRUE,
        .shader_int16 = enabled.shaderInt16 == VK_TRUE,
        .shader_int64 = enabled.shaderInt64 == VK_TRUE,
        .shader_integer_dot_product = vk13.shaderIntegerDotProduct == VK_TRUE,
        .storage_buffer_8bit = vk12.storageBuffer8BitAccess == VK_TRUE,
        .uniform_storage_buffer_8bit = vk12.uniformAndStorageBuffer8BitAccess == VK_TRUE,
        .storage_buffer_16bit = vk11.storageBuffer16BitAccess == VK_TRUE,
        .uniform_storage_buffer_16bit = vk11.uniformAndStorageBuffer16BitAccess == VK_TRUE,
        .image_gather_extended = enabled.shaderImageGatherExtended == VK_TRUE,
        .storage_image_extended_formats = enabled.shaderStorageImageExtendedFormats == VK_TRUE,
        .storage_image_read_without_format = enabled.shaderStorageImageReadWithoutFormat == VK_TRUE,
        .storage_image_write_without_format =
            enabled.shaderStorageImageWriteWithoutFormat == VK_TRUE,
        .compute_shader_derivatives = derivatives,
        .derivative_group_linear = derivatives,
        .derivative_group_quads =
            derivatives && g_derivatives.computeDerivativeGroupQuads == VK_TRUE,
    };
}

void OnDeviceCreated(Device& device) {
    if (Settings::Get().graphics.upscaler == Settings::Upscaler::Dlss) {
        Gr2Fsr3::OnDeviceCreated(device.instance, device.gpu, device.device, device.queue_family,
                                 device.dispatch);
    }
}

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    const Settings::Graphics graphics = Settings::Get().graphics;
    g_upscaler = Settings::Upscaler::Off;
    g_quality = graphics.upscale_quality;
    g_ratio = 1.0f;
    if (graphics.upscaler == Settings::Upscaler::Off) {
        return true;
    }
    Gr2Fsr3::SetDispatcher(device.dispatch);
    const vk::PhysicalDeviceFeatures features = device.gpu.getFeatures(device.dispatch);
    switch (graphics.upscaler) {
    case Settings::Upscaler::Fsr3: {
        FfxVkPortableDeviceCapabilities caps{.structSize = sizeof(caps)};
        if (ffxVkPortableQueryDeviceCapabilities(static_cast<VkPhysicalDevice>(device.gpu),
                                                 &caps) != FFX_VK_PORTABLE_OK ||
            caps.fsr3ComputePrerequisites != VK_TRUE ||
            features.shaderStorageImageWriteWithoutFormat == VK_FALSE) {
            LOG_WARNING(Render_Vulkan, "GR2port upscaler: the GPU cannot run FSR 3.1; the game's "
                                       "antialiasing runs");
            return true;
        }
        break;
    }
    case Settings::Upscaler::Fsr4:
        if (!g_fsr4_features.shader_integer_dot_product ||
            !g_fsr4_features.storage_image_extended_formats) {
            LOG_WARNING(Render_Vulkan, "GR2port upscaler: the GPU lacks integer dot products or "
                                       "extended storage formats for FSR 4; the game's "
                                       "antialiasing runs");
            return true;
        }
        break;
    case Settings::Upscaler::Dlss:
        if (!Gr2Fsr3::DlssRunning()) {
            LOG_WARNING(Render_Vulkan,
                        "GR2port upscaler: DLSS is off: {}; the game's "
                        "antialiasing runs",
                        Gr2Fsr3::DlssBootNotice());
            return true;
        }
        break;
    default:
        return true;
    }
    if (graphics.upscaler != Settings::Upscaler::Fsr3) {
        // The inputs pass: the toned colour, the velocity and the depth in; their copies out.
        const auto& dl = device.dispatch;
        const std::array<vk::DescriptorSetLayoutBinding, 6> bindings{{
            {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {1, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute},
            {3, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {4, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
            {5, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        g_inputs_layout =
            device.device
                .createDescriptorSetLayout({.bindingCount = static_cast<u32>(bindings.size()),
                                            .pBindings = bindings.data()},
                                           nullptr, dl)
                .value;
        const std::array<vk::DescriptorPoolSize, 2> sizes{{
            {vk::DescriptorType::eSampledImage, 3},
            {vk::DescriptorType::eStorageImage, 3},
        }};
        g_inputs_pool = device.device
                            .createDescriptorPool({.maxSets = 1,
                                                   .poolSizeCount = static_cast<u32>(sizes.size()),
                                                   .pPoolSizes = sizes.data()},
                                                  nullptr, dl)
                            .value;
        if (!g_inputs_layout || !g_inputs_pool) {
            return false;
        }
        const auto [result, sets] =
            device.device.allocateDescriptorSets({.descriptorPool = g_inputs_pool,
                                                  .descriptorSetCount = 1,
                                                  .pSetLayouts = &g_inputs_layout},
                                                 dl);
        if (result != vk::Result::eSuccess) {
            return false;
        }
        g_inputs_set = sets[0];
        const std::array set_layouts{g_inputs_layout};
        g_inputs = CreateComputePipeline(device, FSR4_INPUTS_COMP, sizeof(InputsPush), set_layouts);
        if (!g_inputs.pipeline) {
            return false;
        }
    }
    g_upscaler = graphics.upscaler;
    g_ratio = Settings::UpscaleRatio(graphics.upscale_quality);
    LOG_INFO(Render_Vulkan, "GR2port upscaler: {} at {:.2f}x", Name(g_upscaler), g_ratio.load());
    return true;
}

void Stop() {
    if (g_memory != nullptr) {
        FreeContexts();
    }
}

bool Active() {
    return g_upscaler != Settings::Upscaler::Off;
}

vk::Extent2D RenderExtent(vk::Extent2D output) {
    const float ratio = g_ratio.load(std::memory_order_relaxed);
    if (ratio == 1.0f) {
        return output;
    }
    const auto scaled = [ratio](u32 size) {
        return std::max(1u, static_cast<u32>(std::lround(static_cast<float>(size) / ratio)));
    };
    return {scaled(output.width), scaled(output.height)};
}

float LodBias() {
    // AMD's and GR2fork's bias: the size ratio's, and one level more.
    return Active() ? std::log2(1.0f / g_ratio.load()) - 1.0f : 0.0f;
}

bool Resize(vk::Extent2D render, vk::Extent2D output) {
    FreeContexts();
    if (!Active()) {
        return true;
    }
    // The transfer usage is for the clear of DLSS, as on its inputs.
    g_output =
        g_memory->CreateImage(OutputFormat, output,
                              vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled |
                                  vk::ImageUsageFlagBits::eTransferDst,
                              vk::ImageAspectFlagBits::eColor);
    if (!g_output.image || (UsesInputs() && !CreateInputs(render))) {
        return false;
    }
    const auto start = Clock::now();
    bool created = false;
    switch (g_upscaler) {
    case Settings::Upscaler::Fsr3:
        created = CreateFsr3(render, output);
        break;
    case Settings::Upscaler::Fsr4:
        created = CreateFsr4(render, output);
        break;
    case Settings::Upscaler::Dlss:
        created = CreateDlss(render, output);
        break;
    default:
        break;
    }
    if (!created) {
        return true;
    }
    g_render = render;
    g_output_extent = output;
    g_phase_count = PhaseCount(render, output);
    g_phase = 0;
    g_reset = true;
    g_failed_logged = false;
    LOG_INFO(Render_Vulkan, "GR2port upscaler: {} {}x{} -> {}x{}, {} jitter phases, made in {} ms",
             Name(g_upscaler), render.width, render.height, output.width, output.height,
             g_phase_count,
             std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    return true;
}

void SetInputs(vk::Image depth, vk::ImageView depth_view, vk::Image velocity,
               vk::ImageView velocity_view) {
    g_depth = depth;
    g_velocity = velocity;
    if (UsesInputs()) {
        WriteInput(1, vk::DescriptorType::eSampledImage, velocity_view, vk::ImageLayout::eGeneral);
        WriteInput(2, vk::DescriptorType::eSampledImage, depth_view,
                   vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void SetColor(vk::Image toned, vk::ImageView view, vk::Format format) {
    g_toned = toned;
    g_toned_format = format;
    if (UsesInputs()) {
        WriteInput(0, vk::DescriptorType::eSampledImage, view,
                   vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void BeginFrame(const u8* packet, u64 frame, u32 frames_in_flight) {
    g_renderer_frame = frame;
    g_frames_in_flight = frames_in_flight;
    if (!Active()) {
        g_jitter = {};
        return;
    }
    const Clock::time_point now = Clock::now();
    if (g_last_frame != Clock::time_point{}) {
        const auto gap = now - g_last_frame;
        g_reset = g_reset || gap > ResetGap;
        g_frame_ms = std::min(std::chrono::duration<float, std::milli>(gap).count(), MaxFrameMs);
    }
    g_last_frame = now;
    ReadCamera(packet);
    g_phase = (g_phase + 1) % g_phase_count;
    g_jitter = {Halton(g_phase + 1, 2) - 0.5f, Halton(g_phase + 1, 3) - 0.5f};
}

Offset Jitter() {
    return g_jitter;
}

vk::Viewport SceneViewport(vk::Extent2D extent, bool jittered) {
    const Offset jitter = jittered ? g_jitter : Offset{};
    return {
        .x = jitter.x,
        .y = static_cast<float>(extent.height) + jitter.y,
        .width = static_cast<float>(extent.width),
        .height = -static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
}

void Record(vk::CommandBuffer cmd) {
    const bool ready = (g_upscaler == Settings::Upscaler::Fsr3 && g_fsr3 != nullptr) ||
                       (g_upscaler == Settings::Upscaler::Fsr4 && g_fsr4 != nullptr) ||
                       (g_upscaler == Settings::Upscaler::Dlss && g_dlss_ready);
    if (!ready || !g_output.image) {
        return;
    }
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto Depth = vk::ImageAspectFlagBits::eDepth;
    // The upscaler reads the depth as a compute shader's sampled image.
    ImageBarrier(cmd, g_depth, Depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                     vk::PipelineStageFlagBits2::eLateFragmentTests |
                     vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eNone, Compute, vk::AccessFlagBits2::eShaderSampledRead);
    // The last frame's picture read the output.
    ImageBarrier(cmd, g_output.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderSampledRead,
                 Compute, vk::AccessFlagBits2::eShaderStorageWrite);
    const float near_plane = g_camera.near_plane;
    const float far_plane = g_camera.far_plane;
    switch (g_upscaler) {
    case Settings::Upscaler::Fsr3:
        RecordFsr3(cmd, near_plane, far_plane, g_camera.fov);
        break;
    case Settings::Upscaler::Fsr4:
        RecordInputs(cmd, near_plane, far_plane);
        RecordFsr4(cmd);
        break;
    case Settings::Upscaler::Dlss:
        RecordInputs(cmd, near_plane, far_plane);
        RecordDlss(cmd);
        break;
    default:
        break;
    }
    g_reset = false;
    ImageBarrier(cmd, g_depth, Depth, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eDepthStencilReadOnlyOptimal, Compute, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                     vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                     vk::AccessFlagBits2::eShaderSampledRead);
    ImageBarrier(cmd, g_output.image, vk::ImageAspectFlagBits::eColor, vk::ImageLayout::eGeneral,
                 vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderStorageWrite,
                 Compute, vk::AccessFlagBits2::eShaderSampledRead);
}

vk::ImageView OutputView() {
    return g_output.view;
}

} // namespace Gr2Port::Upscaler
