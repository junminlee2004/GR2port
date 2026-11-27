// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "graphics/gr2_fsr3/fsr4_context.h"
#include "graphics/renderer_vulkan/vk_common.h"

namespace Gr2Fsr3 {

/// Preset of DLSS, by the letter of NGX's render preset; Default lets the DLSS library choose.
enum class DlssPreset : u8 { Default, E, J, K, L, M };

/// DLSS quality modes, numbered as NVSDK_NGX_PerfQuality_Value.
enum class DlssMode : u32 {
    Performance = 0,
    Balanced = 1,
    Quality = 2,
    UltraPerformance = 3,
    Dlaa = 5,
};

/// Name of a quality mode.
const char* DlssModeName(DlssMode mode);

/// The render sizes a DLSS mode accepts for an output size.
struct DlssRange {
    vk::Extent2D optimal{};
    vk::Extent2D min{};
    vk::Extent2D max{};
};

/// One DLSS evaluation of the images of a context.
struct DlssEvaluateInfo {
    Fsr4Images images;
    vk::Extent2D render_size{};
    vk::Extent2D output_size{};
    float jitter_x{}; ///< Render pixels, as the viewport jitter.
    float jitter_y{};
    float motion_vector_scale_x{}; ///< Gives the motion in render pixels.
    float motion_vector_scale_y{};
    bool reset{};
};

#if defined(ENABLE_GR2_FSR3) && !defined(__APPLE__)

/// Main thread, before the guest address space is reserved: for a Gravity Rush game with DLSS on,
/// starts CUDA, which NGX needs, with the free space below 512 GiB held, so the space CUDA reserves
/// below 1 TiB stays out of the game's memory.
void StartCudaForDlss();

/**
 * While GR2port's instance extensions are chosen. With DLSS chosen in GR2port's graphics settings,
 * appends the instance extensions NGX needs for DLSS when the DLSS library is in DlssDir() and the
 * instance offers all of them.
 */
void ExtendInstance(std::vector<const char*>& extensions,
                    const vk::detail::DispatchLoaderDynamic& dispatch);

/// Right before GR2port's device is created: appends the device extensions NGX needs for DLSS when
/// the physical device supports DLSS and all of them.
void ExtendDeviceForDlss(vk::Instance instance, vk::PhysicalDevice physical_device,
                         vk::DeviceCreateInfo& create_info,
                         const vk::detail::DispatchLoaderDynamic& dispatch);

/// Right after GR2port's device and dispatch are set up: starts NGX on the device.
void OnDeviceCreated(vk::Instance instance, vk::PhysicalDevice physical_device, vk::Device device,
                     const vk::detail::DispatchLoaderDynamic& dispatch);

/// The folder the DLSS library loads from: dlss in the user folder.
std::filesystem::path DlssDir();

/// Whether NGX runs DLSS on the device. Any thread after the device is created.
bool DlssRunning();

/// Why DLSS does not run.
std::string DlssBootNotice();

/// GPU command thread, while no other DLSS call runs: the render sizes the mode accepts, or empty
/// with `notice` set.
std::optional<DlssRange> DlssOptimalRange(DlssMode mode, DlssPreset preset,
                                          vk::Extent2D output_size, std::string& notice);

/// GPU command thread, while no other DLSS call runs: sets the mode and the preset that the next
/// DlssCreate makes the NGX feature with.
void DlssSetOptions(DlssMode mode, DlssPreset preset);

/// Record thread, with no feature made: makes the NGX feature for the sizes, recording its start-up
/// commands in cmdbuf, which must run before the first evaluation. Returns why it failed, or an
/// empty string.
std::string DlssCreate(vk::CommandBuffer cmdbuf, vk::Extent2D render_size,
                       vk::Extent2D output_size);

/// Record thread, with the feature made: evaluates DLSS into info.images.output; returns why it
/// failed, or an empty string. The inputs are written and in
/// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, and the output is in VK_IMAGE_LAYOUT_GENERAL.
std::string DlssEvaluate(vk::CommandBuffer cmdbuf, const DlssEvaluateInfo& info);

/// Record thread, after the GPU finished every evaluation: releases the NGX feature.
void DlssFree();

#else

inline void StartCudaForDlss() {}
inline void ExtendInstance(std::vector<const char*>&, const vk::detail::DispatchLoaderDynamic&) {}
inline void ExtendDeviceForDlss(vk::Instance, vk::PhysicalDevice, vk::DeviceCreateInfo&,
                                const vk::detail::DispatchLoaderDynamic&) {}
inline void OnDeviceCreated(vk::Instance, vk::PhysicalDevice, vk::Device,
                            const vk::detail::DispatchLoaderDynamic&) {}
inline bool DlssRunning() {
    return false;
}
inline std::string DlssBootNotice() {
    return "DLSS is not in this build";
}
inline std::optional<DlssRange> DlssOptimalRange(DlssMode, DlssPreset, vk::Extent2D, std::string&) {
    return std::nullopt;
}
inline void DlssSetOptions(DlssMode, DlssPreset) {}
inline std::string DlssCreate(vk::CommandBuffer, vk::Extent2D, vk::Extent2D) {
    return "DLSS is not in this build";
}
inline std::string DlssEvaluate(vk::CommandBuffer, const DlssEvaluateInfo&) {
    return "DLSS is not in this build";
}
inline void DlssFree() {}

#endif

} // namespace Gr2Fsr3
