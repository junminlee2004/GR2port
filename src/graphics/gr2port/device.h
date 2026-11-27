// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "graphics/renderer_vulkan/vk_common.h"

struct SDL_Window;

namespace Gr2Port {

/// The renderer's Vulkan instance and device. Every call goes through dispatch.
struct Device {
    vk::detail::DispatchLoaderDynamic dispatch;
    vk::Instance instance;
    vk::SurfaceKHR surface;
    vk::PhysicalDevice gpu;
    vk::Device device;
    vk::Queue queue;
    /// A second queue of the same family for the catcher's probe, or none.
    vk::Queue probe_queue;
    u32 queue_family = 0;
    vk::DriverId driver_id{};
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
    /// The fault comes with the driver's own record of it (deviceFaultVendorBinary).
    bool fault_binary = false;
    /// The driver tracks resources and marks its own checkpoints for that record
    /// (VK_NV_device_diagnostics_config).
    bool driver_diagnostics = false;
    /// With the catcher on, the GPU writes numbers between commands (VK_AMD_buffer_marker).
    bool buffer_marker = false;
    /// A colour attachment's blend enable and equation can be dynamic state
    /// (VK_EXT_extended_dynamic_state3).
    bool dynamic_blend = false;
    /// Swapchains take the HDR metadata of their content (VK_EXT_hdr_metadata).
    bool hdr_metadata = false;
    /// Every pipeline is made through this cache, which a file keeps between runs.
    vk::PipelineCache pipeline_cache;
};

/// Records a barrier on the first mip level and layer of image's aspect.
void ImageBarrier(const Device& device, vk::CommandBuffer cmd, vk::Image image,
                  vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access,
                  vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

/// Creates the instance, a surface for window, and a device on the first discrete GPU (else the
/// first GPU) that has the features the renderer needs and a queue family that draws and presents
/// to the surface.
bool CreateDevice(SDL_Window* window, Device& device);

} // namespace Gr2Port
