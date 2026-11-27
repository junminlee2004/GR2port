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
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "graphics/gr2_fsr3/dlss_ngx.h"
#include "graphics/gr2_fsr3/fsr4_assets.h"
#include "graphics/gr2_fsr3/fsr4_lite_bridge.h"
#include "graphics/gr2_fsr3/shaders/fsr4_inputs_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass0_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass2_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass2_fma_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass5_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass5_fma_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass7_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass7_fma_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass9_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass9_fma_comp.h"
#include "graphics/gr2_fsr3/shaders/fsr4_main_1080_pass9_srgb_comp.h"
#include "graphics/gr2_fsr3/vk_trampolines.h"
#include "graphics/gr2_fsr3/xess_vulkan.h"
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
// FSR 4, DLSS and XeSS read copies of the inputs in these formats.
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
// The SHA-256 of AMD's FSR 4 modules whose results the FSR4_MAIN_1080 modules reproduce, and of
// fsr4_lite_main_initializer.bin, which holds the weights that passes 2, 5 and 9 have compiled in.
constexpr std::array<u8, 32> MainPass0Sha256{
    0xa2, 0x63, 0xf2, 0x09, 0xcc, 0x51, 0x42, 0xcd, 0xd4, 0x69, 0x9a, 0x5e, 0x78, 0x27, 0x8a, 0x90,
    0x3a, 0x7d, 0x79, 0x6f, 0x58, 0x17, 0x7e, 0x06, 0x1e, 0x63, 0xb9, 0xc5, 0xd2, 0xe3, 0xc8, 0xaf,
};
constexpr std::array<u8, 32> MainPass2Sha256{
    0xde, 0x1b, 0x1b, 0x63, 0x5c, 0x54, 0xb8, 0xb2, 0xe3, 0x79, 0x43, 0x18, 0x98, 0xd2, 0x16, 0xf4,
    0xb0, 0xf4, 0xa5, 0xdd, 0x3a, 0xee, 0xbf, 0x61, 0x9a, 0x48, 0x6d, 0x93, 0x03, 0x14, 0x05, 0x9c,
};
constexpr std::array<u8, 32> MainPass5Sha256{
    0x8f, 0x36, 0xeb, 0xe7, 0xc1, 0x45, 0x54, 0xa1, 0xe3, 0x89, 0x06, 0xba, 0x84, 0xe6, 0xf8, 0x1b,
    0xda, 0xed, 0xe6, 0xa1, 0x99, 0xcf, 0xfb, 0xff, 0xbe, 0xb1, 0xa8, 0xee, 0x05, 0xad, 0x18, 0x18,
};
constexpr std::array<u8, 32> MainPass7Sha256{
    0xc3, 0x07, 0x1c, 0x31, 0x30, 0x16, 0x41, 0xcd, 0x57, 0x8d, 0xb5, 0x4d, 0xd6, 0xbb, 0x73, 0x9c,
    0x05, 0x6f, 0x95, 0xe4, 0x13, 0xb8, 0x06, 0x7c, 0x65, 0x24, 0xdf, 0x10, 0x8b, 0xd4, 0xc4, 0x49,
};
constexpr std::array<u8, 32> MainPass9Sha256{
    0xed, 0x47, 0xf0, 0x18, 0x77, 0xa9, 0x5c, 0x55, 0x69, 0x73, 0x70, 0xae, 0x84, 0x85, 0xd3, 0xa1,
    0xdf, 0x60, 0x8d, 0x3e, 0x52, 0x6e, 0xae, 0x85, 0xe4, 0x7d, 0x21, 0xb6, 0x01, 0x25, 0x5c, 0x56,
};
constexpr std::array<u8, 32> MainInitializerSha256{
    0x4b, 0x98, 0x98, 0xda, 0x71, 0x9d, 0xc2, 0xdc, 0xa5, 0xdc, 0x45, 0xfd, 0x71, 0xde, 0xc5, 0xf8,
    0x0b, 0xdb, 0x98, 0x86, 0xa5, 0x80, 0xfe, 0x30, 0x38, 0xaf, 0x59, 0xef, 0xe6, 0x37, 0xd5, 0x5e,
};
// The passes, by AMD's module, that run in 64-wide subgroups where RADV runs them faster.
constexpr std::array<std::pair<std::size_t, const std::array<u8, 32>*>, 5> WideSubgroupPasses{{
    {0, &MainPass0Sha256},
    {2, &MainPass2Sha256},
    {5, &MainPass5Sha256},
    {7, &MainPass7Sha256},
    {9, &MainPass9Sha256},
}};
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
};

// The inputs pass writes 1 - depth with this flag: FSR 4 takes the nearer depth as the smaller.
constexpr u32 InputsDepthInverted = 2;

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
// Whether RenderExtent rounds up, as XeSS's smallest render size does.
std::atomic<bool> g_round_up{false};
Image g_output;
vk::Extent2D g_render;
vk::Extent2D g_output_extent;
vk::Image g_depth;
vk::ImageView g_depth_view;
vk::Image g_velocity;
vk::ImageView g_velocity_view;
vk::Image g_toned;
vk::ImageView g_toned_view;
vk::Format g_toned_format = vk::Format::eUndefined;
s32 g_phase = 0;
s32 g_phase_count = 1;
Offset g_jitter;
Camera g_camera;
bool g_camera_seen = false;
float g_frame_ms = FirstFrameMs;
Clock::time_point g_last_frame{};
bool g_reset = true;
// The frame's camera cut or came after a gap.
bool g_cut = false;
u64 g_frame_id = 0;
u64 g_renderer_frame = 0;
u32 g_frames_in_flight = 1;
bool g_failed_logged = false;

// FSR 3.1.
FfxVkPortableUpscaleContext* g_fsr3 = nullptr;
// What the context was made with, which each dispatch is checked against.
FfxVkPortableUpscaleCreateInfo g_fsr3_create{};
u64 g_fsr3_logged_issues = 0;

// FSR 4, DLSS and XeSS: the inputs pass and its copies of the inputs.
vk::DescriptorSetLayout g_inputs_layout;
vk::DescriptorPool g_inputs_pool;
vk::DescriptorSet g_inputs_set;
Pipeline g_inputs;
Image g_input_color;
Image g_input_motion;
Image g_input_depth;

// FSR 4: the features the device was made with, the context and its unretired dispatches.
vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR g_derivatives{};
vk::PhysicalDeviceCooperativeVectorFeaturesNV g_cooperative_vector{};
std::vector<const char*> g_device_extensions;
// The device takes derivatives in compute shaders, as FSR 4 and FSR 3.1.6's frame generation do.
bool g_compute_derivatives = false;
Gr2Fsr3::Fsr4DeviceFeatures g_fsr4_features{};
// Whether the device runs cooperative vectors of signed bytes, as the FSR4_MAIN_1080 modules do.
bool g_fsr4_cooperative_vectors = false;
Gr2Fsr4Context* g_fsr4 = nullptr;
std::deque<Fsr4Frame> g_fsr4_frames;
// Whether FSR 4 reads the toned colour, the velocity and the depth, not the inputs pass's copies.
bool g_fsr4_reads_scene = false;
// Whether FSR 4's pass 9 can write the picture's sRGB, and whether it does.
bool g_fsr4_can_encode = false;
bool g_fsr4_encodes = false;
// Whether the picture is sharpened after the upscale, which reads the output image.
bool g_sharpened = false;

// DLSS: whether NGX's feature is made for the sizes.
bool g_dlss_ready = false;
// DLSS: the commands NGX recorded when it made the feature, which run before its first evaluation,
// and whether they still wait for the queue (the splash has it before the game starts).
vk::CommandPool g_start_pool;
vk::CommandBuffer g_start_commands;
vk::Fence g_start_fence;
bool g_start_pending = false;
// Whether a setting needs the context made again at its sizes.
bool g_remake = false;

// XeSS: whether the context is initialized for the sizes.
bool g_xess_ready = false;

const char* Name(Settings::Upscaler upscaler) {
    switch (upscaler) {
    case Settings::Upscaler::Fsr3:
        return "FSR 3.1";
    case Settings::Upscaler::Fsr4:
        return "FSR 4";
    case Settings::Upscaler::Dlss:
        return "DLSS";
    case Settings::Upscaler::Xess:
        return "XeSS";
    default:
        return "off";
    }
}

bool UsesInputs() {
    return (g_upscaler == Settings::Upscaler::Fsr4 && !g_fsr4_reads_scene) ||
           g_upscaler == Settings::Upscaler::Dlss || g_upscaler == Settings::Upscaler::Xess;
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

// The XeSS setting that renders at the quality's ratio.
Gr2Fsr3::XessQuality XessQualityOf(Settings::UpscaleQuality quality) {
    switch (quality) {
    case Settings::UpscaleQuality::Native:
        return Gr2Fsr3::XessQuality::Aa;
    case Settings::UpscaleQuality::Balanced:
        return Gr2Fsr3::XessQuality::Quality;
    case Settings::UpscaleQuality::Performance:
        return Gr2Fsr3::XessQuality::Balanced;
    case Settings::UpscaleQuality::UltraPerformance:
        return Gr2Fsr3::XessQuality::UltraPerformance;
    default:
        return Gr2Fsr3::XessQuality::UltraQuality;
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
    g_fsr4_reads_scene = false;
    g_fsr4_can_encode = false;
    g_fsr4_encodes = false;
    if (g_dlss_ready) {
        Gr2Fsr3::DlssFree();
        g_dlss_ready = false;
    }
    // Commands never run that name the freed feature's images.
    g_start_pending = false;
    g_xess_ready = false;
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
        .getDeviceProcAddr = Gr2Fsr3::DeviceProcAddr(),
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

bool HasSha256(const void* data, std::size_t size, const std::array<u8, 32>& expected) {
    std::array<u8, EVP_MAX_MD_SIZE> digest{};
    return EVP_Digest(data, size, digest.data(), nullptr, EVP_sha256(), nullptr) == 1 &&
           std::equal(expected.begin(), expected.end(), digest.begin());
}

// Row-major copies of the weights that the cooperative vector modules read after AMD's model data,
// per output channel the 16 input channel bytes of each tap in turn: pass 2's 3x3 and stride-2
// weights, then the 3x3 weights of passes 5 and 7.
std::vector<u8> VectorMatrices(const std::vector<u8>& model) {
    std::vector<u8> matrices;
    const auto add = [&](std::size_t base, u32 outputs, u32 taps, std::size_t tap_bytes) {
        for (u32 output = 0; output < outputs; ++output) {
            for (u32 tap = 0; tap < taps; ++tap) {
                const auto row = model.begin() + base + tap_bytes * tap + 16 * output;
                matrices.insert(matrices.end(), row, row + 16);
            }
        }
    };
    add(1152, 16, 9, 256);
    add(4864, 32, 4, 512);
    add(7040, 16, 9, 256);
    add(13952, 16, 9, 256);
    return matrices;
}

// The weights that the float modules read after AMD's model data, a region per pass in the order
// 5, 7, 2, 9. Each layer's weights and biases are divided by the power of two its results keep,
// and its biases take back the 128 the modules add to the bytes they read from the scratch buffer.
std::vector<float> FloatWeights(const std::vector<u8>& model) {
    std::vector<float> floats;
    const auto weight = [&model](std::size_t offset) {
        return static_cast<double>(static_cast<s8>(model[offset]));
    };
    const auto bias = [&model](std::size_t offset) {
        s32 value;
        std::memcpy(&value, model.data() + offset, sizeof(value));
        return static_cast<double>(value);
    };
    const auto add = [&floats](double value) { floats.push_back(static_cast<float>(value)); };
    // Channels 0-15 are a tensor's first plane and 16-31 its second.
    const auto planes = [](double first, double second) {
        return [=](u32 channel) { return channel < 16 ? first : second; };
    };
    // A 3x3 layer, 16 to 16 channels, [tap][output][input] at base + 256 tap + 16 output + input.
    const auto three_by_three = [&](std::size_t base, std::size_t biases, double divisor) {
        for (u32 tap = 0; tap < 9; ++tap) {
            for (u32 output = 0; output < 16; ++output) {
                for (u32 input = 0; input < 16; ++input) {
                    add(weight(base + 256 * tap + 16 * output + input) / divisor);
                }
            }
        }
        for (u32 output = 0; output < 16; ++output) {
            double sum = 0.0;
            for (u32 tap = 0; tap < 9; ++tap) {
                for (u32 input = 0; input < 16; ++input) {
                    sum += weight(base + 256 * tap + 16 * output + input);
                }
            }
            add((bias(biases + 4 * output) - 128.0 * sum) / divisor);
        }
    };
    // A 1x1 layer, [output][input] at base + inputs * output + input. The inputs from scratch on
    // come from the scratch buffer, and residual(output) is what the input added to the output's
    // results gains from the modules' offset.
    const auto one_by_one = [&](std::size_t base, u32 outputs, u32 inputs, std::size_t biases,
                                u32 scratch, const auto& divisor, const auto& residual) {
        for (u32 output = 0; output < outputs; ++output) {
            for (u32 input = 0; input < inputs; ++input) {
                add(weight(base + inputs * output + input) / divisor(output));
            }
        }
        for (u32 output = 0; output < outputs; ++output) {
            double sum = 0.0;
            for (u32 input = scratch; input < inputs; ++input) {
                sum += weight(base + inputs * output + input);
            }
            add((bias(biases + 4 * output) - 128.0 * sum) / divisor(output) - residual(output));
        }
    };
    three_by_three(7040, 9344, 256.0);
    one_by_one(9472, 64, 32, 11520, 16, planes(256.0, 256.0), planes(0.0, 0.0));
    one_by_one(11776, 32, 64, 13824, 64, planes(64.0, 128.0), planes(64.0, 64.0));

    three_by_three(13952, 16256, 256.0);
    one_by_one(16384, 64, 32, 18432, 16, planes(128.0, 128.0), planes(0.0, 0.0));
    one_by_one(18688, 32, 64, 20736, 64, planes(64.0, 64.0), planes(128.0, 256.0));
    for (u32 index = 0; index < 64 * 32; ++index) {
        add(weight(20864 + index) / 256.0);
    }
    for (u32 output = 0; output < 16; ++output) {
        add(bias(22912 + 4 * output) / 256.0 - 128.0);
    }

    three_by_three(1152, 3456, 128.0);
    one_by_one(3584, 32, 16, 4096, 16, planes(512.0, 512.0), planes(0.0, 0.0));
    one_by_one(4224, 16, 32, 4736, 32, planes(64.0, 64.0), planes(64.0, 64.0));
    // The stride-2 layer, [output][block pixel][input] at 4864 + 512 pixel + 16 output + input.
    const auto stride_divisor = planes(64.0, 128.0);
    for (u32 output = 0; output < 32; ++output) {
        for (u32 index = 0; index < 64; ++index) {
            add(weight(4864 + 512 * (index / 16) + 16 * output + index % 16) /
                stride_divisor(output));
        }
    }
    for (u32 output = 0; output < 32; ++output) {
        add(bias(6912 + 4 * output) / stride_divisor(output));
    }

    three_by_three(23040, 25344, 256.0);
    one_by_one(25472, 32, 16, 25984, 16, planes(256.0, 256.0), planes(0.0, 0.0));
    one_by_one(26112, 16, 32, 26624, 32, planes(64.0, 64.0), planes(64.0, 64.0));
    for (u32 index = 0; index < 512; ++index) {
        add(weight(26752 + index) / 1024.0);
    }
    for (u32 index = 0; index < 8; ++index) {
        add(bias(27264 + 4 * index) / 1024.0);
    }
    return floats;
}

bool CreateFsr4(vk::Extent2D render, vk::Extent2D output) {
    const auto properties = g_device->gpu.getProperties2<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
        vk::PhysicalDeviceShaderIntegerDotProductProperties, vk::PhysicalDeviceDriverProperties>(
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
        Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "fsr4_lite";
    const Gr2Fsr3::Fsr4LoadResult loaded =
        Gr2Fsr3::LoadFsr4Assets(folder, Fsr4Preset(g_quality), output, limits);
    if (!loaded.assets) {
        TurnOff(fmt::format("FSR 4 has no usable files in {}: {}",
                            Common::FS::PathToUTF8String(folder), loaded.notice));
        return false;
    }
    const Gr2Fsr3::Fsr4Assets& assets = *loaded.assets;
    // Passes 2, 5 and 7 have the same results on cooperative vectors, which run on NVIDIA's
    // tensor cores.
    const auto matches = [&assets](std::size_t pass, const std::array<u8, 32>& sha256) {
        const std::vector<u32>& words = assets.shaders[pass];
        return HasSha256(words.data(), words.size() * sizeof(u32), sha256);
    };
    const bool main_weights =
        HasSha256(assets.initializer.data(), assets.initializer.size(), MainInitializerSha256);
    const bool vector_pass2 =
        g_fsr4_cooperative_vectors && main_weights && matches(2, MainPass2Sha256);
    const bool vector_pass5 =
        g_fsr4_cooperative_vectors && main_weights && matches(5, MainPass5Sha256);
    const bool vector_pass7 = g_fsr4_cooperative_vectors && matches(7, MainPass7Sha256);
    // Passes 2, 5, 7 and 9 also have the same results from float multiply-adds of weights made
    // from this initializer, which GPUs without 8-bit dot product instructions run faster.
    const auto& dot_products =
        properties.get<vk::PhysicalDeviceShaderIntegerDotProductProperties>();
    const bool float_products = !g_fsr4_cooperative_vectors && main_weights &&
                                !dot_products.integerDotProduct4x8BitPackedSignedAccelerated;
    const bool float_pass2 = float_products && matches(2, MainPass2Sha256);
    const bool float_pass5 = float_products && matches(5, MainPass5Sha256);
    const bool float_pass7 = float_products && matches(7, MainPass7Sha256);
    const bool float_pass9 = float_products && matches(9, MainPass9Sha256);
    const bool radv =
        properties.get<vk::PhysicalDeviceDriverProperties>().driverID == vk::DriverId::eMesaRadv;
    // Pass 0 has the same results with its weights read as 16-bit floats, which RADV runs faster on
    // the GPUs that do not take the float passes. It reads the scene's images, so FSR 4 needs no
    // copies of them.
    const bool half_pass0 = radv && dot_products.integerDotProduct4x8BitPackedSignedAccelerated &&
                            matches(0, MainPass0Sha256);
    g_fsr4_reads_scene = half_pass0;
    // RDNA3 and later, the GPUs RADV reports mixed-sign 8-bit dot products on, run the passes in
    // WideSubgroupPasses faster in 64-wide subgroups.
    const bool wide_subgroups =
        radv && dot_products.integerDotProduct4x8BitPackedMixedSignednessAccelerated;
    // GR2port's pass 9 has the same results. It takes the log of each input colour texel once for
    // the four pixels a thread makes, which RADV runs faster than AMD's module in 32-wide
    // subgroups, and it can write the picture's sRGB itself, which saves the encode pass when the
    // picture is not sharpened.
    g_fsr4_can_encode = radv && dot_products.integerDotProduct4x8BitPackedSignedAccelerated &&
                        main_weights && matches(9, MainPass9Sha256);
    g_fsr4_encodes = g_fsr4_can_encode && !g_sharpened;
    const bool shared_pass9 = g_fsr4_can_encode && !g_fsr4_encodes && !wide_subgroups;
    std::vector<u8> initializer = assets.initializer;
    if (vector_pass2 || vector_pass5 || vector_pass7) {
        const std::vector<u8> matrices = VectorMatrices(assets.initializer);
        initializer.insert(initializer.end(), matrices.begin(), matrices.end());
    }
    if (float_pass2 || float_pass5 || float_pass7 || float_pass9) {
        const std::vector<float> floats = FloatWeights(assets.initializer);
        const auto* bytes = reinterpret_cast<const u8*>(floats.data());
        initializer.insert(initializer.end(), bytes, bytes + floats.size() * sizeof(float));
    }
    Gr2Fsr4CreateInfo info{
        .device = static_cast<VkDevice>(g_device->device),
        .physical_device = static_cast<VkPhysicalDevice>(g_device->gpu),
        .initializer = initializer.data(),
        .initializer_size = initializer.size(),
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
    std::string replaced_passes;
    const auto replace = [&](std::size_t pass, const u32* words, std::size_t size_bytes) {
        info.shaders[pass] =
            Gr2Fsr4Shader{.words = words, .size_bytes = size_bytes, .entry_point = "main"};
        replaced_passes += fmt::format(" {}", pass);
    };
    if (half_pass0) {
        replace(0, FSR4_MAIN_1080_PASS0_COMP, sizeof(FSR4_MAIN_1080_PASS0_COMP));
    }
    if (vector_pass2) {
        replace(2, FSR4_MAIN_1080_PASS2_COMP, sizeof(FSR4_MAIN_1080_PASS2_COMP));
    }
    if (vector_pass5) {
        replace(5, FSR4_MAIN_1080_PASS5_COMP, sizeof(FSR4_MAIN_1080_PASS5_COMP));
    }
    if (vector_pass7) {
        replace(7, FSR4_MAIN_1080_PASS7_COMP, sizeof(FSR4_MAIN_1080_PASS7_COMP));
    }
    if (float_pass2) {
        replace(2, FSR4_MAIN_1080_PASS2_FMA_COMP, sizeof(FSR4_MAIN_1080_PASS2_FMA_COMP));
    }
    if (float_pass5) {
        replace(5, FSR4_MAIN_1080_PASS5_FMA_COMP, sizeof(FSR4_MAIN_1080_PASS5_FMA_COMP));
    }
    if (float_pass7) {
        replace(7, FSR4_MAIN_1080_PASS7_FMA_COMP, sizeof(FSR4_MAIN_1080_PASS7_FMA_COMP));
    }
    if (float_pass9) {
        replace(9, FSR4_MAIN_1080_PASS9_FMA_COMP, sizeof(FSR4_MAIN_1080_PASS9_FMA_COMP));
    }
    if (g_fsr4_encodes) {
        replace(9, FSR4_MAIN_1080_PASS9_SRGB_COMP, sizeof(FSR4_MAIN_1080_PASS9_SRGB_COMP));
    }
    if (shared_pass9) {
        replace(9, FSR4_MAIN_1080_PASS9_COMP, sizeof(FSR4_MAIN_1080_PASS9_COMP));
    }
    if (!replaced_passes.empty()) {
        LOG_INFO(Render_Vulkan, "GR2port upscaler: FSR 4 passes{} run on {}", replaced_passes,
                 g_fsr4_cooperative_vectors ? "cooperative vectors"
                 : float_products           ? "float multiply-adds"
                                            : "GR2port's modules");
    }
    // These passes have the same results in 64-wide subgroups.
    if (wide_subgroups) {
        std::string wide_passes;
        for (const auto& [pass, sha256] : WideSubgroupPasses) {
            if (matches(pass, *sha256)) {
                info.shaders[pass].subgroup_size = 64;
                wide_passes += fmt::format(" {}", pass);
            }
        }
        if (!wide_passes.empty()) {
            LOG_INFO(Render_Vulkan, "GR2port upscaler: FSR 4 passes{} run in 64-wide subgroups",
                     wide_passes);
        }
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
        TurnOff(fmt::format("DLSS {} takes {}x{} to {}x{} for {}x{}, not {}x{}",
                            Gr2Fsr3::DlssModeName(mode), range->min.width, range->min.height,
                            range->max.width, range->max.height, output.width, output.height,
                            render.width, render.height));
        return false;
    }
    Gr2Fsr3::DlssSetOptions(mode, DlssModel);
    const auto& dl = g_device->dispatch;
    if (g_device->device.resetCommandPool(g_start_pool, {}, dl) != vk::Result::eSuccess ||
        g_start_commands.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl) !=
            vk::Result::eSuccess) {
        TurnOff("DLSS has no command buffer for its start-up commands");
        return false;
    }
    const std::string failed = Gr2Fsr3::DlssCreate(g_start_commands, render, output);
    // Everything after them on the queue sees what NGX's start-up commands wrote.
    const vk::MemoryBarrier2 written{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    g_start_commands.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &written}, dl);
    if (g_start_commands.end(dl) != vk::Result::eSuccess || !failed.empty()) {
        Gr2Fsr3::DlssFree();
        TurnOff(failed.empty() ? "DLSS's start-up commands were not recorded" : failed);
        return false;
    }
    g_start_pending = true;
    g_dlss_ready = true;
    LOG_INFO(Render_Vulkan, "GR2port upscaler: DLSS runs preset E, the convolutional model");
    return true;
}

bool CreateXess(vk::Extent2D render, vk::Extent2D output) {
    if (const std::string rejected = Gr2Fsr3::XessInit(XessQualityOf(g_quality), render, output,
                                                       0.5f * static_cast<float>(render.width),
                                                       -0.5f * static_cast<float>(render.height));
        !rejected.empty()) {
        TurnOff(rejected);
        return false;
    }
    g_xess_ready = true;
    return true;
}

bool Holds() {
    return g_fsr3 != nullptr || g_fsr4 != nullptr || g_dlss_ready || g_xess_ready;
}

// With the GPU idle: makes the context and the images for the sizes, freeing the old ones. False
// when an image finds no room.
bool Make(vk::Extent2D render, vk::Extent2D output) {
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
    if (!g_output.image) {
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
    case Settings::Upscaler::Xess:
        created = CreateXess(render, output);
        break;
    default:
        break;
    }
    if (!created) {
        return true;
    }
    // After the context, which tells whether FSR 4 reads the copies.
    if (UsesInputs() && !CreateInputs(render)) {
        return false;
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

// With the GPU idle and the queue the renderer's: runs DLSS's start-up commands, if they wait.
void SubmitStart() {
    if (!g_start_pending) {
        return;
    }
    g_start_pending = false;
    const auto& dl = g_device->dispatch;
    const vk::CommandBufferSubmitInfo commands{.commandBuffer = g_start_commands};
    const vk::SubmitInfo2 submit{.commandBufferInfoCount = 1, .pCommandBufferInfos = &commands};
    if (g_device->device.resetFences(g_start_fence, dl) != vk::Result::eSuccess ||
        g_device->queue.submit2(submit, g_start_fence, dl) != vk::Result::eSuccess ||
        g_device->device.waitForFences(g_start_fence, true, UINT64_MAX, dl) !=
            vk::Result::eSuccess) {
        TurnOff("DLSS's start-up commands did not run");
    }
}

// The commands DLSS's feature records when NGX makes it, and their fence.
bool CreateStartCommands(const Device& device) {
    const auto& dl = device.dispatch;
    g_start_pool = device.device
                       .createCommandPool({.flags = vk::CommandPoolCreateFlagBits::eTransient,
                                           .queueFamilyIndex = device.queue_family},
                                          nullptr, dl)
                       .value;
    g_start_fence = device.device.createFence({}, nullptr, dl).value;
    if (!g_start_pool || !g_start_fence) {
        return false;
    }
    const auto [result, buffers] =
        device.device.allocateCommandBuffers({.commandPool = g_start_pool,
                                              .level = vk::CommandBufferLevel::ePrimary,
                                              .commandBufferCount = 1},
                                             dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_start_commands = buffers[0];
    return true;
}

// Copies the toned colour, the velocity and the depth into the images FSR 4, DLSS and XeSS read.
void RecordInputs(vk::CommandBuffer cmd) {
    const auto& dl = g_device->dispatch;
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    for (const Image* image : {&g_input_color, &g_input_motion, &g_input_depth}) {
        // The last frame's upscaler read the image; DLSS may have cleared it.
        ImageBarrier(*g_device, cmd, image->image, vk::ImageLayout::eUndefined,
                     vk::ImageLayout::eGeneral, Compute | vk::PipelineStageFlagBits2::eClear,
                     vk::AccessFlagBits2::eTransferWrite, Compute,
                     vk::AccessFlagBits2::eShaderStorageWrite);
    }
    const InputsPush push{
        .size = {g_render.width, g_render.height},
        .flags = g_upscaler == Settings::Upscaler::Fsr4 ? InputsDepthInverted : 0u,
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_inputs.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, g_inputs.layout, 0, g_inputs_set, {},
                           dl);
    cmd.pushConstants(g_inputs.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push,
                      dl);
    cmd.dispatch((g_render.width + InputGroupPixels - 1) / InputGroupPixels,
                 (g_render.height + InputGroupPixels - 1) / InputGroupPixels, 1, dl);
    for (const Image* image : {&g_input_color, &g_input_motion, &g_input_depth}) {
        ImageBarrier(*g_device, cmd, image->image, vk::ImageLayout::eGeneral,
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
        g_cut = true;
    }
    g_camera = {.near_plane = near_plane, .far_plane = far_plane, .fov = fov};
    g_camera_seen = true;
}

void RecordFsr3(vk::CommandBuffer cmd) {
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
        .cameraNear = g_camera.near_plane,
        .cameraFar = g_camera.far_plane,
        .cameraVerticalFovRadians = g_camera.fov,
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

void RecordFsr4(vk::CommandBuffer cmd, vk::Image picture, vk::ImageView picture_view) {
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
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
    };
    // The scene's images are the render size, in the layouts the inputs pass reads them in.
    const auto scene = [](vk::Image source, vk::ImageView view, vk::ImageLayout layout) {
        return Gr2Fsr4Image{
            .image = static_cast<VkImage>(source),
            .view = static_cast<VkImageView>(view),
            .width = g_render.width,
            .height = g_render.height,
            .layout = static_cast<VkImageLayout>(layout),
        };
    };
    constexpr auto Read = vk::ImageLayout::eShaderReadOnlyOptimal;
    const Gr2Fsr4DispatchInfo info{
        .command_buffer = static_cast<VkCommandBuffer>(cmd),
        .frame_id = g_frame_id,
        .color = g_fsr4_reads_scene ? scene(g_toned, g_toned_view, Read) : image(g_input_color),
        .depth = g_fsr4_reads_scene ? scene(g_depth, g_depth_view, Read) : image(g_input_depth),
        .motion_vectors = g_fsr4_reads_scene
                              ? scene(g_velocity, g_velocity_view, vk::ImageLayout::eGeneral)
                              : image(g_input_motion),
        .output = g_fsr4_encodes ? Gr2Fsr4Image{.image = static_cast<VkImage>(picture),
                                                .view = static_cast<VkImageView>(picture_view),
                                                .width = g_output_extent.width,
                                                .height = g_output_extent.height,
                                                .layout = VK_IMAGE_LAYOUT_GENERAL}
                                 : image(g_output),
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

// The copies of the inputs and the output, as DLSS and XeSS take them.
Gr2Fsr3::Fsr4Images InputImages() {
    const auto image = [](const Image& source) {
        return Gr2Fsr3::Fsr4Image{.image = source.image, .view = source.view};
    };
    return {
        .color = image(g_input_color),
        .motion_vectors = image(g_input_motion),
        .depth = image(g_input_depth),
        .output = image(g_output),
    };
}

// Moves the copies of the inputs to the layout DLSS and XeSS read them in.
void MoveInputsToRead(vk::CommandBuffer cmd) {
    for (const Image* image : {&g_input_color, &g_input_motion, &g_input_depth}) {
        ImageBarrier(*g_device, cmd, image->image, vk::ImageLayout::eGeneral,
                     vk::ImageLayout::eShaderReadOnlyOptimal,
                     vk::PipelineStageFlagBits2::eComputeShader,
                     vk::AccessFlagBits2::eShaderStorageWrite,
                     vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    }
}

void RecordDlss(vk::CommandBuffer cmd) {
    MoveInputsToRead(cmd);
    const Gr2Fsr3::DlssEvaluateInfo info{
        .images = InputImages(),
        .render_size = g_render,
        .output_size = g_output_extent,
        .jitter_x = g_jitter.x,
        .jitter_y = g_jitter.y,
        // NGX takes the motion in render pixels: half the width or height per clip-space unit.
        .motion_vector_scale_x = 0.5f * static_cast<float>(g_render.width),
        .motion_vector_scale_y = -0.5f * static_cast<float>(g_render.height),
        .reset = g_reset,
    };
    if (const std::string failed = Gr2Fsr3::DlssEvaluate(cmd, info); !failed.empty()) {
        LogFailureOnce(failed);
    }
}

void RecordXess(vk::CommandBuffer cmd) {
    MoveInputsToRead(cmd);
    const Gr2Fsr3::XessExecuteInfo info{
        .images = InputImages(),
        .render_size = g_render,
        .output_size = g_output_extent,
        .jitter_x = g_jitter.x,
        .jitter_y = g_jitter.y,
        .reset = g_reset,
    };
    if (const std::string failed = Gr2Fsr3::XessExecute(cmd, info); !failed.empty()) {
        LogFailureOnce(failed);
    }
}

} // namespace

void ExtendInstance(std::vector<const char*>& extensions, u32 api_version,
                    const vk::detail::DispatchLoaderDynamic& dispatch) {
    Gr2Fsr3::ExtendInstance(extensions, dispatch);
    if (Settings::Get().graphics.upscaler == Settings::Upscaler::Xess) {
        Gr2Fsr3::ExtendInstanceForXess(extensions, api_version);
    }
}

void ExtendDevice(vk::Instance instance, vk::PhysicalDevice gpu,
                  const vk::detail::DispatchLoaderDynamic& dispatch, vk::DeviceCreateInfo& info,
                  vk::PhysicalDeviceFeatures2& features, vk::PhysicalDeviceVulkan11Features& vk11,
                  vk::PhysicalDeviceVulkan12Features& vk12,
                  vk::PhysicalDeviceVulkan13Features& vk13) {
    const Settings::Graphics graphics = Settings::Get().graphics;
    const Settings::Upscaler chosen = graphics.upscaler;
    if (chosen == Settings::Upscaler::Off && !graphics.frame_generation) {
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
    // FSR 3.1's frame generation writes and reads them.
    if (graphics.frame_generation) {
        features.features.shaderStorageImageReadWithoutFormat =
            core.shaderStorageImageReadWithoutFormat;
    }
    const auto [result, extensions] = gpu.enumerateDeviceExtensionProperties(nullptr, dispatch);
    const bool has_derivatives =
        result == vk::Result::eSuccess &&
        std::ranges::any_of(extensions, [](const vk::ExtensionProperties& extension) {
            return std::string_view{extension.extensionName.data()} ==
                   VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
        });
    // The create info points at these until the device is made.
    g_device_extensions.assign(info.ppEnabledExtensionNames,
                               info.ppEnabledExtensionNames + info.enabledExtensionCount);
    // FSR 3.1.6's frame generation and FSR 4 Lite take derivatives in compute shaders.
    bool derivatives = false;
    if (has_derivatives && (graphics.frame_generation || chosen == Settings::Upscaler::Fsr4)) {
        const auto queried =
            gpu.getFeatures2<vk::PhysicalDeviceFeatures2,
                             vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>(dispatch)
                .get<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
        derivatives = queried.computeDerivativeGroupLinear == VK_TRUE;
        if (derivatives) {
            g_derivatives = vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR{
                .pNext = features.pNext,
                .computeDerivativeGroupQuads = queried.computeDerivativeGroupQuads,
                .computeDerivativeGroupLinear = VK_TRUE,
            };
            features.pNext = &g_derivatives;
            g_device_extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        }
    }
    g_compute_derivatives = derivatives;
    info.enabledExtensionCount = static_cast<u32>(g_device_extensions.size());
    info.ppEnabledExtensionNames = g_device_extensions.data();
    if (chosen == Settings::Upscaler::Dlss) {
        Gr2Fsr3::ExtendDeviceForDlss(instance, gpu, info, dispatch);
        return;
    }
    if (chosen == Settings::Upscaler::Xess) {
        Gr2Fsr3::ExtendDeviceForXess(instance, gpu, info);
        return;
    }
    if (chosen != Settings::Upscaler::Fsr4) {
        return;
    }
    // What the FSR 4 Lite shaders can declare, as far as the GPU has it; every pass runs 32 wide.
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
    vk13.subgroupSizeControl =
        supported.get<vk::PhysicalDeviceVulkan13Features>().subgroupSizeControl;
    const bool has_cooperative_vectors =
        result == vk::Result::eSuccess &&
        std::ranges::any_of(extensions, [](const vk::ExtensionProperties& extension) {
            return std::string_view{extension.extensionName.data()} ==
                   VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME;
        });
    // Cooperative vector modules use the Vulkan memory model.
    g_fsr4_cooperative_vectors = false;
    if (has_cooperative_vectors && supported12.vulkanMemoryModel == VK_TRUE) {
        const auto queried =
            gpu.getFeatures2<vk::PhysicalDeviceFeatures2,
                             vk::PhysicalDeviceCooperativeVectorFeaturesNV>(dispatch)
                .get<vk::PhysicalDeviceCooperativeVectorFeaturesNV>();
        const auto [properties_result, combinations] =
            gpu.getCooperativeVectorPropertiesNV(dispatch);
        g_fsr4_cooperative_vectors =
            queried.cooperativeVector == VK_TRUE && properties_result == vk::Result::eSuccess &&
            std::ranges::any_of(combinations, [](const vk::CooperativeVectorPropertiesNV& types) {
                return types.inputType == vk::ComponentTypeKHR::eUint32 &&
                       types.inputInterpretation == vk::ComponentTypeKHR::eSint8PackedNV &&
                       types.matrixInterpretation == vk::ComponentTypeKHR::eSint8 &&
                       types.biasInterpretation == vk::ComponentTypeKHR::eSint32 &&
                       types.resultType == vk::ComponentTypeKHR::eSint32;
            });
    }
    if (g_fsr4_cooperative_vectors) {
        g_cooperative_vector = vk::PhysicalDeviceCooperativeVectorFeaturesNV{
            .pNext = features.pNext,
            .cooperativeVector = VK_TRUE,
        };
        features.pNext = &g_cooperative_vector;
        vk12.vulkanMemoryModel = VK_TRUE;
        g_device_extensions.push_back(VK_NV_COOPERATIVE_VECTOR_EXTENSION_NAME);
    }
    info.enabledExtensionCount = static_cast<u32>(g_device_extensions.size());
    info.ppEnabledExtensionNames = g_device_extensions.data();
    const vk::PhysicalDeviceFeatures& enabled = features.features;
    g_fsr4_features = Gr2Fsr3::Fsr4DeviceFeatures{
        .shader_float16 = vk12.shaderFloat16 == VK_TRUE,
        .shader_float64 = enabled.shaderFloat64 == VK_TRUE,
        .shader_int8 = vk12.shaderInt8 == VK_TRUE,
        .shader_int16 = enabled.shaderInt16 == VK_TRUE,
        .shader_int64 = enabled.shaderInt64 == VK_TRUE,
        .shader_integer_dot_product = vk13.shaderIntegerDotProduct == VK_TRUE,
        .subgroup_size_control = vk13.subgroupSizeControl == VK_TRUE,
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

bool ComputeDerivatives() {
    return g_compute_derivatives;
}

void OnDeviceCreated(Device& device) {
    if (Settings::Get().graphics.upscaler == Settings::Upscaler::Dlss) {
        Gr2Fsr3::OnDeviceCreated(device.instance, device.gpu, device.device, device.dispatch);
    }
}

bool Start(const Device& device, Memory& memory, vk::Extent2D picture) {
    g_device = &device;
    g_memory = &memory;
    const Settings::Graphics graphics = Settings::Get().graphics;
    g_upscaler = Settings::Upscaler::Off;
    g_quality = graphics.upscale_quality;
    g_sharpened = graphics.upscale_sharpening > 0;
    g_ratio = 1.0f;
    if (graphics.upscaler == Settings::Upscaler::Off) {
        return true;
    }
    Gr2Fsr3::SetDispatcher(device.dispatch);
    Gr2Fsr3::SetPipelineCache(static_cast<VkPipelineCache>(device.pipeline_cache));
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
            !g_fsr4_features.storage_image_extended_formats ||
            !g_fsr4_features.subgroup_size_control) {
            LOG_WARNING(Render_Vulkan, "GR2port upscaler: the GPU lacks integer dot products, "
                                       "extended storage formats or subgroup size control for "
                                       "FSR 4; the game's antialiasing runs");
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
        if (!CreateStartCommands(device)) {
            LOG_WARNING(Render_Vulkan, "GR2port upscaler: DLSS is off: no command buffer for its "
                                       "start-up commands; the game's antialiasing runs");
            return true;
        }
        break;
    case Settings::Upscaler::Xess:
        if (const std::string notice = Gr2Fsr3::XessCreate(device.instance, device.gpu,
                                                           device.device, device.pipeline_cache);
            !notice.empty()) {
            LOG_WARNING(Render_Vulkan,
                        "GR2port upscaler: XeSS is off: {}; the game's antialiasing runs", notice);
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
        g_inputs_layout = CreateSetLayout(device, bindings);
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
    g_round_up = g_upscaler == Settings::Upscaler::Xess;
    g_ratio = Settings::UpscaleRatio(graphics.upscale_quality);
    LOG_INFO(Render_Vulkan, "GR2port upscaler: {} at {:.2f}x", Name(g_upscaler), g_ratio.load());
    // Made before the game starts for the picture the game will name, so the pipelines go into the
    // pipeline cache and DLSS makes its feature here; the scene's first resize keeps the context.
    Make(RenderExtent(picture), picture);
    return true;
}

void Stop() {
    if (g_memory != nullptr) {
        FreeContexts();
    }
    if (g_start_pool) {
        g_device->device.destroyCommandPool(g_start_pool, nullptr, g_device->dispatch);
        g_device->device.destroyFence(g_start_fence, nullptr, g_device->dispatch);
        g_start_pool = nullptr;
        g_start_fence = nullptr;
    }
    Gr2Fsr3::XessDestroy();
}

bool Active() {
    return g_upscaler != Settings::Upscaler::Off;
}

vk::Extent2D RenderExtent(vk::Extent2D output) {
    const float ratio = g_ratio.load(std::memory_order_relaxed);
    if (ratio == 1.0f) {
        return output;
    }
    const bool round_up = g_round_up.load(std::memory_order_relaxed);
    const auto scaled = [ratio, round_up](u32 size) {
        const float exact = static_cast<float>(size) / ratio;
        return std::max(1u, static_cast<u32>(round_up ? std::ceil(exact) : std::round(exact)));
    };
    return {scaled(output.width), scaled(output.height)};
}

float LodBias() {
    // AMD's and GR2fork's bias: the size ratio's, and one level more.
    return Active() ? std::log2(1.0f / g_ratio.load()) - 1.0f : 0.0f;
}

bool Resize(vk::Extent2D render, vk::Extent2D output) {
    // The context made before the game started, or at the last resize, serves the same sizes.
    if (Holds() && render == g_render && output == g_output_extent && !g_remake) {
        g_phase = 0;
        g_reset = true;
        g_failed_logged = false;
    } else if (!Make(render, output)) {
        return false;
    }
    g_remake = false;
    SubmitStart();
    return true;
}

void SetInputs(vk::Image depth, vk::ImageView depth_view, vk::Image velocity,
               vk::ImageView velocity_view) {
    g_depth = depth;
    g_depth_view = depth_view;
    g_velocity = velocity;
    g_velocity_view = velocity_view;
    if (UsesInputs()) {
        WriteInput(1, vk::DescriptorType::eSampledImage, velocity_view, vk::ImageLayout::eGeneral);
        WriteInput(2, vk::DescriptorType::eSampledImage, depth_view,
                   vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void SetColor(vk::Image toned, vk::ImageView view, vk::Format format) {
    g_toned = toned;
    g_toned_view = view;
    g_toned_format = format;
    if (UsesInputs()) {
        WriteInput(0, vk::DescriptorType::eSampledImage, view,
                   vk::ImageLayout::eShaderReadOnlyOptimal);
    }
}

void SetFrame(u64 frame, u32 frames_in_flight) {
    g_renderer_frame = frame;
    g_frames_in_flight = frames_in_flight;
}

void BeginFrame(const u8* packet) {
    // Frame generation reads the camera and the time without an upscaler too.
    const Clock::time_point now = Clock::now();
    g_cut = false;
    if (g_last_frame != Clock::time_point{}) {
        const auto gap = now - g_last_frame;
        g_cut = gap > ResetGap;
        g_reset = g_reset || g_cut;
        g_frame_ms = std::min(std::chrono::duration<float, std::milli>(gap).count(), MaxFrameMs);
    }
    g_last_frame = now;
    ReadCamera(packet);
    if (!Active()) {
        g_jitter = {};
        return;
    }
    g_phase = (g_phase + 1) % g_phase_count;
    g_jitter = {Halton(g_phase + 1, 2) - 0.5f, Halton(g_phase + 1, 3) - 0.5f};
}

Offset Jitter() {
    return g_jitter;
}

View FrameView() {
    return {
        .near_plane = g_camera.near_plane,
        .far_plane = g_camera.far_plane,
        .fov = g_camera.fov,
        .frame_ms = g_frame_ms,
        .cut = g_cut,
    };
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

bool Record(vk::CommandBuffer cmd, vk::Image picture, vk::ImageView picture_view) {
    const bool ready = (g_upscaler == Settings::Upscaler::Fsr3 && g_fsr3 != nullptr) ||
                       (g_upscaler == Settings::Upscaler::Fsr4 && g_fsr4 != nullptr) ||
                       (g_upscaler == Settings::Upscaler::Dlss && g_dlss_ready) ||
                       (g_upscaler == Settings::Upscaler::Xess && g_xess_ready);
    if (!ready || !g_output.image) {
        return false;
    }
    constexpr auto Compute = vk::PipelineStageFlagBits2::eComputeShader;
    constexpr auto Depth = vk::ImageAspectFlagBits::eDepth;
    // The upscaler reads the depth as a compute shader's sampled image.
    ImageBarrier(*g_device, cmd, g_depth, vk::ImageLayout::eDepthStencilReadOnlyOptimal,
                 vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                     vk::PipelineStageFlagBits2::eLateFragmentTests |
                     vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eNone, Compute, vk::AccessFlagBits2::eShaderSampledRead,
                 Depth);
    // The last frame's picture read the output, which DLSS may have cleared, and clears again on a
    // reset.
    constexpr auto Clear = vk::PipelineStageFlagBits2::eClear;
    constexpr auto Cleared = vk::AccessFlagBits2::eTransferWrite;
    ImageBarrier(*g_device, cmd, g_output.image, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eGeneral, Compute | Clear,
                 vk::AccessFlagBits2::eShaderSampledRead | Cleared, Compute | Clear,
                 vk::AccessFlagBits2::eShaderStorageWrite | Cleared);
    switch (g_upscaler) {
    case Settings::Upscaler::Fsr3:
        RecordFsr3(cmd);
        break;
    case Settings::Upscaler::Fsr4:
        if (!g_fsr4_reads_scene) {
            RecordInputs(cmd);
        }
        RecordFsr4(cmd, picture, picture_view);
        break;
    case Settings::Upscaler::Dlss:
        RecordInputs(cmd);
        RecordDlss(cmd);
        break;
    case Settings::Upscaler::Xess:
        RecordInputs(cmd);
        RecordXess(cmd);
        break;
    default:
        break;
    }
    g_reset = false;
    ImageBarrier(*g_device, cmd, g_depth, vk::ImageLayout::eShaderReadOnlyOptimal,
                 vk::ImageLayout::eDepthStencilReadOnlyOptimal, Compute, vk::AccessFlagBits2::eNone,
                 vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                     vk::PipelineStageFlagBits2::eFragmentShader,
                 vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                     vk::AccessFlagBits2::eShaderSampledRead,
                 Depth);
    ImageBarrier(*g_device, cmd, g_output.image, vk::ImageLayout::eGeneral,
                 vk::ImageLayout::eGeneral, Compute, vk::AccessFlagBits2::eShaderStorageWrite,
                 Compute, vk::AccessFlagBits2::eShaderSampledRead);
    return g_upscaler == Settings::Upscaler::Fsr4 && g_fsr4_encodes;
}

bool SetSharpened(bool sharpened) {
    const bool remake = g_fsr4 != nullptr && g_fsr4_can_encode && g_fsr4_encodes == sharpened;
    g_sharpened = sharpened;
    g_remake = g_remake || remake;
    return remake;
}

vk::ImageView OutputView() {
    return g_output.view;
}

} // namespace Gr2Port::Upscaler
