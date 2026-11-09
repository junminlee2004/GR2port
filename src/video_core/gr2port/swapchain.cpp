// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <span>

#include "common/logging/log.h"
#include "video_core/gr2port/settings.h"
#include "video_core/gr2port/swapchain.h"

namespace Gr2Port {

namespace {

vk::SurfaceFormatKHR PickFormat(const std::vector<vk::SurfaceFormatKHR>& formats) {
    for (const vk::SurfaceFormatKHR& f : formats) {
        if ((f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm) &&
            f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return f;
        }
    }
    return formats.front();
}

// Without VSync frames show at once, double buffering waits for the display, and triple buffering
// replaces the frame that waits. FIFO is the one mode every device has.
vk::PresentModeKHR PickMode(const std::vector<vk::PresentModeKHR>& modes) {
    static constexpr std::array NoVSync{vk::PresentModeKHR::eImmediate,
                                        vk::PresentModeKHR::eMailbox};
    static constexpr std::array TripleBuffered{vk::PresentModeKHR::eMailbox};
    std::span<const vk::PresentModeKHR> preferred;
    switch (Settings::Get().display.vsync) {
    case Settings::VSync::Off:
        preferred = NoVSync;
        break;
    case Settings::VSync::TripleBuffered:
        preferred = TripleBuffered;
        break;
    case Settings::VSync::DoubleBuffered:
        break;
    }
    for (const vk::PresentModeKHR wanted : preferred) {
        if (std::ranges::find(modes, wanted) != modes.end()) {
            return wanted;
        }
    }
    return vk::PresentModeKHR::eFifo;
}

} // namespace

bool Swapchain::Create(u32 width, u32 height) {
    const auto& dl = d.dispatch;
    const auto [caps_result, caps] = d.gpu.getSurfaceCapabilitiesKHR(d.surface, dl);
    const auto [formats_result, formats] = d.gpu.getSurfaceFormatsKHR(d.surface, dl);
    const auto [modes_result, modes] = d.gpu.getSurfacePresentModesKHR(d.surface, dl);
    if (caps_result != vk::Result::eSuccess || formats_result != vk::Result::eSuccess ||
        modes_result != vk::Result::eSuccess || formats.empty()) {
        LOG_ERROR(Render_Vulkan, "GR2port: no surface properties");
        return false;
    }
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }
    const vk::SurfaceFormatKHR surface_format = PickFormat(formats);
    u32 image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    const vk::SwapchainKHR old = swapchain;
    const vk::SwapchainCreateInfoKHR info{
        .surface = d.surface,
        .minImageCount = image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc,
        .preTransform = caps.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = PickMode(modes),
        .clipped = true,
        .oldSwapchain = old,
    };
    const auto [result, created] = d.device.createSwapchainKHR(info, nullptr, dl);
    Destroy();
    if (old) {
        d.device.destroySwapchainKHR(old, nullptr, dl);
    }
    swapchain = nullptr;
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: no swapchain: {}", vk::to_string(result));
        return false;
    }
    swapchain = created;
    format = surface_format.format;
    images = d.device.getSwapchainImagesKHR(swapchain, dl).value;
    for (const vk::Image image : images) {
        const vk::ImageViewCreateInfo view_info{
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        views.push_back(d.device.createImageView(view_info, nullptr, dl).value);
        rendered.push_back(d.device.createSemaphore({}, nullptr, dl).value);
    }
    LOG_INFO(Render_Vulkan, "GR2port: swapchain {}x{}, {} images, {}", extent.width, extent.height,
             images.size(), vk::to_string(info.presentMode));
    return true;
}

void Swapchain::Destroy() {
    const auto& dl = d.dispatch;
    for (const vk::ImageView view : views) {
        d.device.destroyImageView(view, nullptr, dl);
    }
    for (const vk::Semaphore semaphore : rendered) {
        d.device.destroySemaphore(semaphore, nullptr, dl);
    }
    views.clear();
    rendered.clear();
    images.clear();
}

bool Swapchain::Acquire(vk::Semaphore acquired, u32& index) {
    if (!swapchain) {
        return false;
    }
    const auto [result, image] =
        d.device.acquireNextImageKHR(swapchain, UINT64_MAX, acquired, nullptr, d.dispatch);
    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        return false;
    }
    index = image;
    return true;
}

bool Swapchain::Present(u32 index) {
    const vk::PresentInfoKHR info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered[index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
    };
    const vk::Result result = d.queue.presentKHR(info, d.dispatch);
    return result == vk::Result::eSuccess;
}

} // namespace Gr2Port
