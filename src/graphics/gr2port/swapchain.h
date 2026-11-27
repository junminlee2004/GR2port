// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "graphics/gr2port/device.h"

namespace Gr2Port {

/// The window's swapchain: immediate presentation without VSync where the surface has it, else
/// FIFO. Each image has its own semaphore for the end of the frame drawn into it.
class Swapchain {
public:
    explicit Swapchain(const Device& device) : d{device} {}

    /// Creates the swapchain at the window's size, replacing the current one. The device must
    /// be idle.
    bool Create(u32 width, u32 height);

    /// Acquires the next image, signalling acquired; false when the swapchain needs creating.
    bool Acquire(vk::Semaphore acquired, u32& index);

    /// Presents image index after its rendered semaphore, with present id where it is not 0;
    /// false when the swapchain needs creating.
    bool Present(u32 index, u64 id = 0);
    /// If hdr is true, Create makes an HDR10 swapchain where the surface offers one.
    void SetHdr(bool hdr) {
        hdr_wanted = hdr;
    }

    /// If two is true, Create makes room for two images acquired at once, as frame generation's
    /// two presents a frame take.
    void SetTwoAcquired(bool two) {
        two_wanted = two;
    }

    /// Two images can be acquired at once.
    bool TakesTwo() const {
        return takes_two;
    }

    /// Without VSync: each image shows as soon as it is presented.
    bool Immediate() const {
        return immediate;
    }
    /// The images take HDR10: ST 2084 of BT.2020 light.
    bool Hdr() const {
        return hdr;
    }

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
    bool hdr_wanted = false;
    bool hdr = false;
    bool two_wanted = false;
    bool takes_two = false;
    bool immediate = false;
    vk::Extent2D extent;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> views;
    std::vector<vk::Semaphore> rendered;
};

} // namespace Gr2Port
