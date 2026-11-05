// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2_pt/device.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Gr2Pt {

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
};

enum class Placement {
    // Device-local memory the CPU never touches.
    Device,
    // Host-visible, coherent memory the CPU writes in order and the GPU reads.
    Upload,
    // Host-cached memory the GPU writes and the CPU reads.
    Readback,
};

/// The renderer's GPU memory, allocated with its own allocator on its own device.
class Memory {
public:
    explicit Memory(const Device& device) : d{device} {}

    bool Create();

    /// A buffer the GPU reads by device address as well as by binding.
    Buffer CreateBuffer(u64 size, vk::BufferUsageFlags usage, Placement placement);
    void Destroy(Buffer& buffer);

    /// A 2D image with one mip level and a view of all of it.
    Image CreateImage(vk::Format format, vk::Extent2D extent, vk::ImageUsageFlags usage,
                      vk::ImageAspectFlags aspect);
    void Destroy(Image& image);

private:
    const Device& d;
    VmaAllocator allocator = nullptr;
};

} // namespace Gr2Pt
