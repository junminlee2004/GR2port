// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "video_core/gr2port/device.h"

namespace Gr2Port {

/// The window's swapchain: mailbox presentation when the surface has it, else immediate, else
/// FIFO. Each image has its own semaphore for the end of the frame drawn into it.
class Swapchain {
public:
    explicit Swapchain(const Device& device) : d{device} {}

    /// Creates the swapchain at the window's size, replacing the current one. The device must
    /// be idle.
    bool Create(u32 width, u32 height);

    /// Acquires the next image, signalling acquired; false when the swapchain needs creating.
    bool Acquire(vk::Semaphore acquired, u32& index);

    /// Presents image index after its rendered semaphore; false when the swapchain needs
    /// creating.
    bool Present(u32 index);

    vk::Format Format() const {
        return format;
    }
    vk::Extent2D Extent() const {
        return extent;
    }
    vk::Image Image(u32 index) const {
        return images[index];
    }
    vk::ImageView View(u32 index) const {
        return views[index];
    }
    vk::Semaphore Rendered(u32 index) const {
        return rendered[index];
    }

private:
    void Destroy();

    const Device& d;
    vk::SwapchainKHR swapchain;
    vk::Format format = vk::Format::eUndefined;
    vk::Extent2D extent;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> views;
    std::vector<vk::Semaphore> rendered;
};

} // namespace Gr2Port
