// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include "common/logging/log.h"
#include "graphics/gr2port/device_lost.h"

namespace Gr2Port::DeviceLost {

namespace {

constexpr std::array<const char*, static_cast<u32>(Pass::Count)> PassNames{
    "frame start",      "deformers",    "kernels",
    "simulation",       "point lights", "mesh sampling",
    "self translucent", "shadows",      "fog volumes",
    "surfaces",         "lines",        "character depth",
    "occlusion",        "velocity",     "light",
    "characters",       "sky",          "contours",
    "effects behind",   "glass",        "water",
    "effects in front", "fog apply",    "particles",
    "effects top-most", "meter",        "bloom",
    "antialias",        "composite",    "ui",
    "present",
};

// A marker holds the frame above the pass.
constexpr u32 PassBits = 8;

u64 g_frame = 0;
bool g_reported = false;

void ReportCheckpoints(const Device& d) {
    if (!d.checkpoints) {
        LOG_ERROR(Render_Vulkan, "GR2port: no checkpoints on this GPU");
        return;
    }
    const auto& dl = d.dispatch;
    u32 count = 0;
    dl.vkGetQueueCheckpointData2NV(d.queue, &count, nullptr);
    std::vector<VkCheckpointData2NV> points(count, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV});
    dl.vkGetQueueCheckpointData2NV(d.queue, &count, points.data());
    LOG_ERROR(Render_Vulkan, "GR2port: {} checkpoints; the frame recorded last is {}", count,
              g_frame);
    for (u32 i = 0; i < count; i++) {
        const u64 marker = reinterpret_cast<u64>(points[i].pCheckpointMarker);
        const u64 pass = marker & ((u64{1} << PassBits) - 1);
        LOG_ERROR(Render_Vulkan, "GR2port: stage {:#x} reached frame {}, {}", points[i].stage,
                  marker >> PassBits, pass < PassNames.size() ? PassNames[pass] : "nothing");
    }
}

void ReportFault(const Device& d) {
    if (!d.device_fault) {
        LOG_ERROR(Render_Vulkan, "GR2port: no fault information on this GPU");
        return;
    }
    const auto& dl = d.dispatch;
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (dl.vkGetDeviceFaultInfoEXT(d.device, &counts, nullptr) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2port: the fault's counts failed");
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = addresses.data(),
        .pVendorInfos = vendors.data(),
    };
    // The vendor's binary is not asked for, so a driver that has one answers incomplete and
    // still fills in the rest.
    const u64 binary_bytes = counts.vendorBinarySize;
    counts.vendorBinarySize = 0;
    if (const VkResult result = dl.vkGetDeviceFaultInfoEXT(d.device, &counts, &info);
        result != VK_SUCCESS && result != VK_INCOMPLETE) {
        LOG_ERROR(Render_Vulkan, "GR2port: the fault's information failed: {}",
                  vk::to_string(static_cast<vk::Result>(result)));
        return;
    }
    addresses.resize(std::min<size_t>(addresses.size(), counts.addressInfoCount));
    vendors.resize(std::min<size_t>(vendors.size(), counts.vendorInfoCount));
    LOG_ERROR(Render_Vulkan,
              "GR2port: fault \"{}\", {} addresses, {} vendor records, a vendor binary of {} bytes",
              info.description, addresses.size(), vendors.size(), binary_bytes);
    for (const VkDeviceFaultAddressInfoEXT& address : addresses) {
        LOG_ERROR(Render_Vulkan, "GR2port: fault address {:#x}, within {:#x}, {}",
                  address.reportedAddress, address.addressPrecision,
                  vk::to_string(static_cast<vk::DeviceFaultAddressTypeEXT>(address.addressType)));
    }
    for (const VkDeviceFaultVendorInfoEXT& vendor : vendors) {
        LOG_ERROR(Render_Vulkan, "GR2port: fault record \"{}\", code {:#x}, data {:#x}",
                  vendor.description, vendor.vendorFaultCode, vendor.vendorFaultData);
    }
}

} // namespace

void Mark(const Device& device, vk::CommandBuffer cmd, Pass pass) {
    if (pass == Pass::FrameStart) {
        g_frame++;
    }
    if (device.checkpoints) {
        const u64 marker = (g_frame << PassBits) | static_cast<u64>(pass);
        cmd.setCheckpointNV(reinterpret_cast<const void*>(marker), device.dispatch);
    }
}

void Check(const Device& device, vk::Result result) {
    if (result != vk::Result::eErrorDeviceLost || g_reported) {
        return;
    }
    g_reported = true;
    LOG_ERROR(Render_Vulkan, "GR2port: the device is lost");
    ReportCheckpoints(device);
    ReportFault(device);
    // The emulated GPU's device is lost too, and its assertion ends the process.
    Common::Log::Flush();
}

} // namespace Gr2Port::DeviceLost
