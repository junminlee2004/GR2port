// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/logging/log.h"
#include "graphics/gr2port/device_lost.h"
#include "graphics/gr2port/low_latency.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/swapchain.h"
#include "graphics/gr2port/vblank.h"

namespace Gr2Port {

namespace {

vk::SurfaceFormatKHR PickFormat(const std::vector<vk::SurfaceFormatKHR>& formats, bool hdr) {
    if (hdr) {
        for (const vk::SurfaceFormatKHR& f : formats) {
            if ((f.format == vk::Format::eA2B10G10R10UnormPack32 ||
                 f.format == vk::Format::eA2R10G10B10UnormPack32) &&
                f.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT) {
                return f;
            }
        }
        LOG_WARNING(Render_Vulkan, "GR2port: the window takes no HDR10, so it shows SDR");
    }
    for (const vk::SurfaceFormatKHR& f : formats) {
        if ((f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm) &&
            f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return f;
        }
    }
    return formats.front();
}

// Without VSync frames show at once where the surface allows it. Double and triple buffering
// queue frames for the display (FIFO), the one mode every device has.
vk::PresentModeKHR PickMode(const std::vector<vk::PresentModeKHR>& modes, Settings::VSync vsync) {
    if (vsync == Settings::VSync::Off &&
        std::ranges::find(modes, vk::PresentModeKHR::eImmediate) != modes.end()) {
        return vk::PresentModeKHR::eImmediate;
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
    const vk::SurfaceFormatKHR surface_format = PickFormat(formats, hdr_wanted);
    // Double and triple buffering keep two and three images where the surface allows that many;
    // without VSync the swapchain takes one more than the surface's least.
    const Settings::VSync vsync = Settings::Get().display.vsync;
    u32 image_count = caps.minImageCount + 1;
    if (vsync == Settings::VSync::DoubleBuffered) {
        image_count = std::max(caps.minImageCount, 2u);
    } else if (vsync == Settings::VSync::TripleBuffered) {
        image_count = std::max(caps.minImageCount, 3u);
    }
    // An application may hold as many images at once as the swapchain has beyond the least.
    if (two_wanted) {
        image_count = std::max(image_count, caps.minImageCount + 2);
    }
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    const vk::SwapchainKHR old = swapchain;
    // Reflex's markers and sleep name a swapchain made for them.
    LowLatency::SwapchainGone();
    vk::SwapchainLatencyCreateInfoNV latency{.latencyModeEnable = true};
    const vk::SwapchainCreateInfoKHR info{
        .pNext = LowLatency::ReflexSwapchain() ? &latency : nullptr,
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
        .presentMode = PickMode(modes, vsync),
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
    LowLatency::SwapchainMade(swapchain);
    format = surface_format.format;
    hdr = surface_format.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT;
    immediate = info.presentMode == vk::PresentModeKHR::eImmediate;
    if (hdr && d.hdr_metadata) {
        const Settings::Display display = Settings::Get().display;
        const vk::HdrMetadataEXT metadata{
            .displayPrimaryRed = {0.708f, 0.292f},
            .displayPrimaryGreen = {0.170f, 0.797f},
            .displayPrimaryBlue = {0.131f, 0.046f},
            .whitePoint = {0.3127f, 0.3290f},
            .maxLuminance = static_cast<float>(display.hdr_peak_nits),
            .maxContentLightLevel = static_cast<float>(display.hdr_peak_nits),
            .maxFrameAverageLightLevel = static_cast<float>(display.hdr_paper_white_nits),
        };
        d.device.setHdrMetadataEXT(swapchain, metadata, dl);
    }
    images = d.device.getSwapchainImagesKHR(swapchain, dl).value;
    takes_two = two_wanted && images.size() >= caps.minImageCount + 2;
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
    LOG_INFO(Render_Vulkan, "GR2port: swapchain {}x{}, {} images, {}, {}", extent.width,
             extent.height, images.size(), vk::to_string(info.presentMode), hdr ? "HDR10" : "SDR");
    Vblank::SetSynced(info.presentMode != vk::PresentModeKHR::eImmediate);
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
    DeviceLost::Check(d, result, "the swapchain's next image");
    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        return false;
    }
    index = image;
    return true;
}

bool Swapchain::Present(u32 index, u64 id) {
    const vk::PresentIdKHR present_id{.swapchainCount = 1, .pPresentIds = &id};
    const vk::PresentInfoKHR info{
        .pNext = id != 0 ? &present_id : nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered[index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
    };
    const vk::Result result = d.queue.presentKHR(info, d.dispatch);
    DeviceLost::Check(d, result, "the present");
    return result == vk::Result::eSuccess;
}

} // namespace Gr2Port
