// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

struct SDL_Window;

namespace Gr2Port {

/// The renderer's own Vulkan instance and device, apart from the emulated GPU's. Every call goes
/// through dispatch: the default dispatcher holds the emulated GPU's device.
struct Device {
    vk::detail::DispatchLoaderDynamic dispatch;
    vk::Instance instance;
    vk::SurfaceKHR surface;
    vk::PhysicalDevice gpu;
    vk::Device device;
    vk::Queue queue;
    u32 queue_family = 0;
    /// Nanoseconds per timestamp tick, or 0 when the queue has no timestamps.
    double timestamp_period = 0.0;
    /// The device reports the process's memory use and budget (VK_EXT_memory_budget).
    bool memory_budget = false;
    bool geometry_shaders = false;
    bool tessellation_shaders = false;
    /// Storage images may have formats such as R8 (shaderStorageImageExtendedFormats).
    bool storage_extended_formats = false;
    /// A lost device reports the last checkpoints (VK_NV_device_diagnostic_checkpoints).
    bool checkpoints = false;
    /// A lost device reports the fault (VK_EXT_device_fault).
    bool device_fault = false;
    /// A colour attachment's blend enable and equation can be dynamic state
    /// (VK_EXT_extended_dynamic_state3).
    bool dynamic_blend = false;
    /// Every pipeline is made through this cache, which a file keeps between runs.
    vk::PipelineCache pipeline_cache;
};

/// Creates the instance, a surface for window, and a device on the first discrete GPU (else the
/// first GPU) with a queue family that draws and presents to the surface.
bool CreateDevice(SDL_Window* window, Device& device);

} // namespace Gr2Port
