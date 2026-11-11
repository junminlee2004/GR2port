// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "common/types.h"
#include "video_core/gr2_fsr3/dlss_streamline.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Gr2Fsr3 {

/// Features the Vulkan device was created with that FSR 4 v07 shaders can need.
struct Fsr4DeviceFeatures {
    bool shader_float16{};
    bool shader_float64{};
    bool shader_int8{};
    bool shader_int16{};
    bool shader_int64{};
    bool shader_integer_dot_product{};
    bool storage_buffer_8bit{};
    bool uniform_storage_buffer_8bit{};
    bool storage_buffer_16bit{};
    bool uniform_storage_buffer_16bit{};
    bool image_gather_extended{};
    bool storage_image_extended_formats{};
    bool storage_image_read_without_format{};
    bool storage_image_write_without_format{};
    bool compute_shader_derivatives{}; ///< VK_KHR_compute_shader_derivatives is enabled.
    bool derivative_group_linear{};
    bool derivative_group_quads{};
};

#ifdef ENABLE_GR2_FSR3

void ExtendDevice(vk::PhysicalDevice physical_device, vk::DeviceCreateInfo& create_info,
                  vk::PhysicalDeviceFeatures2& features, vk::PhysicalDeviceVulkan11Features& vk11,
                  vk::PhysicalDeviceVulkan12Features& vk12,
                  vk::PhysicalDeviceVulkan13Features& vk13);

/// The features ExtendDevice recorded; empty unless it ran for one of the titles with its key set.
std::optional<Fsr4DeviceFeatures> Fsr4EnabledFeatures();

#endif

} // namespace Gr2Fsr3
