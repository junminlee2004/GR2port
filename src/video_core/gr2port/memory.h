// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Gr2Port {

struct Buffer {
    vk::Buffer buffer;
    VmaAllocation allocation = nullptr;
    /// The CPU's view of a host-visible buffer, else null.
    u8* mapped = nullptr;
    vk::DeviceAddress address = 0;
    u64 size = 0;
};

struct Image {
    vk::Image image;
    vk::ImageView view;
    VmaAllocation allocation = nullptr;
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent;
    /// The bytes of its memory.
    u64 size = 0;
};

enum class Placement {
    // Device-local memory the CPU never touches.
    Device,
    // Host-visible, coherent memory the CPU writes in order and the GPU reads.
    Upload,
    // As Upload, in system memory: copies and load-time shaders read it once.
    Staging,
    // Host-cached memory the GPU writes and the CPU reads.
    Readback,
};

/// The renderer's GPU memory, allocated with its own allocator on its own device.
class Memory {
public:
    explicit Memory(const Device& device) : d{device} {}

    bool Create();

    /// Device-local memory: the process's use and budget (both renderers share the process),
    /// and the renderer's own use, as of the last Refresh.
    struct Budget {
        u64 usage;
        u64 budget;
        u64 own;
    };
    /// Render thread, once per frame: reads the budget again.
    void Refresh(u64 frame);
    Budget DeviceBudget() const {
        return device_budget;
    }
    /// Whether bytes more device-local memory leave the process a headroom under its budget;
    /// if so, they count as used until the next Refresh.
    bool Reserve(u64 bytes);

    /// A buffer the GPU reads by device address as well as by binding.
    Buffer CreateBuffer(u64 size, vk::BufferUsageFlags usage, Placement placement);
    void Destroy(Buffer& buffer);

    /// A 2D image with layers layers of levels mip levels, and a view of all of it (an array view
    /// when there are several layers).
    Image CreateImage(vk::Format format, vk::Extent2D extent, vk::ImageUsageFlags usage,
                      vk::ImageAspectFlags aspect, u32 layers = 1, u32 levels = 1,
                      vk::ImageCreateFlags flags = {});
    /// A 3D image of one mip level, and a 3D view of it.
    Image CreateVolume(vk::Format format, vk::Extent3D extent, vk::ImageUsageFlags usage);
    /// A sampled 2D image with levels mip levels, which copies fill, and a view of all of it
    /// with mapping. A cube has six layers and a cube view.
    Image CreateTexture(vk::Format format, vk::Extent2D extent, u32 levels,
                        vk::ComponentMapping mapping, bool cube = false);
    void Destroy(Image& image);

private:
    const Device& d;
    VmaAllocator allocator = nullptr;
    Budget device_budget{};
    u64 reserved = 0;
    bool logged_full = false;
};

} // namespace Gr2Port
