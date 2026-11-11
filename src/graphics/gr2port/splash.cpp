// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <stb_image.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/splash.h"
#include "graphics/gr2port/swapchain.h"
#include "graphics/gr2port/window.h"

namespace Gr2Port::Splash {

namespace {

void Barrier(const Device& d, vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from,
             vk::ImageLayout to, vk::AccessFlags2 src_access, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = src_access,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         d.dispatch);
}

// The picture at the largest size that fits the window, centred.
vk::ImageBlit Fit(vk::Extent2D picture, vk::Extent2D window) {
    const float scale = std::min(static_cast<float>(window.width) / picture.width,
                                 static_cast<float>(window.height) / picture.height);
    const s32 width = static_cast<s32>(picture.width * scale);
    const s32 height = static_cast<s32>(picture.height * scale);
    const s32 left = (static_cast<s32>(window.width) - width) / 2;
    const s32 top = (static_cast<s32>(window.height) - height) / 2;
    const vk::ImageSubresourceLayers layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    return vk::ImageBlit{
        .srcSubresource = layers,
        .srcOffsets = {{vk::Offset3D{0, 0, 0}, vk::Offset3D{static_cast<s32>(picture.width),
                                                            static_cast<s32>(picture.height), 1}}},
        .dstSubresource = layers,
        .dstOffsets = {{vk::Offset3D{left, top, 0}, vk::Offset3D{left + width, top + height, 1}}},
    };
}

} // namespace

void Show(const Device& d, Memory& memory, Swapchain& swapchain, vk::CommandPool pool,
          vk::CommandBuffer cmd, vk::Fence done, vk::Semaphore acquired) {
    const std::vector<u8>& png = Common::ElfInfo::Instance().GetSplashData();
    int width = 0;
    int height = 0;
    stbi_uc* const pixels = png.empty()
                                ? nullptr
                                : stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                                        &width, &height, nullptr, 4);
    const Window::Size window = Window::PixelSize();
    if (pixels == nullptr || window.width == 0 || window.height == 0 ||
        !swapchain.Create(window.width, window.height)) {
        LOG_INFO(Render_Vulkan, "GR2port: no splash");
        stbi_image_free(pixels);
        return;
    }
    const vk::Extent2D extent{static_cast<u32>(width), static_cast<u32>(height)};
    const u64 bytes = u64{extent.width} * extent.height * 4;
    Buffer staging =
        memory.CreateBuffer(bytes, vk::BufferUsageFlagBits::eTransferSrc, Placement::Staging);
    // Sampled only because CreateImage makes a view, which a transfer-only image may not have.
    Image picture = memory.CreateImage(vk::Format::eR8G8B8A8Unorm, extent,
                                       vk::ImageUsageFlagBits::eTransferDst |
                                           vk::ImageUsageFlagBits::eTransferSrc |
                                           vk::ImageUsageFlagBits::eSampled,
                                       vk::ImageAspectFlagBits::eColor);
    if (staging.mapped != nullptr) {
        std::memcpy(staging.mapped, pixels, bytes);
    }
    stbi_image_free(pixels);
    u32 index = 0;
    if (staging.mapped == nullptr || !picture.image || !swapchain.Acquire(acquired, index)) {
        memory.Destroy(staging);
        memory.Destroy(picture);
        return;
    }

    const auto& dl = d.dispatch;
    (void)d.device.resetFences(done, dl);
    (void)d.device.resetCommandPool(pool, {}, dl);
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    Barrier(d, cmd, picture.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eNone,
            vk::AccessFlagBits2::eTransferWrite);
    const vk::BufferImageCopy copy{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {extent.width, extent.height, 1},
    };
    cmd.copyBufferToImage(staging.buffer, picture.image, vk::ImageLayout::eTransferDstOptimal, copy,
                          dl);
    Barrier(d, cmd, picture.image, vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
            vk::AccessFlagBits2::eTransferRead);
    const vk::Image target = swapchain.Image(index);
    Barrier(d, cmd, target, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageSubresourceRange whole{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    cmd.clearColorImage(target, vk::ImageLayout::eTransferDstOptimal,
                        vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}}, whole, dl);
    Barrier(d, cmd, target, vk::ImageLayout::eTransferDstOptimal,
            vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
            vk::AccessFlagBits2::eTransferWrite);
    cmd.blitImage(picture.image, vk::ImageLayout::eTransferSrcOptimal, target,
                  vk::ImageLayout::eTransferDstOptimal, Fit(extent, swapchain.Extent()),
                  vk::Filter::eLinear, dl);
    Barrier(d, cmd, target, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eNone);
    (void)cmd.end(dl);

    const vk::SemaphoreSubmitInfo wait{
        .semaphore = acquired,
        .stageMask = vk::PipelineStageFlagBits2::eAllTransfer,
    };
    const vk::SemaphoreSubmitInfo signal{
        .semaphore = swapchain.Rendered(index),
        .stageMask = vk::PipelineStageFlagBits2::eAllTransfer,
    };
    const vk::CommandBufferSubmitInfo commands{.commandBuffer = cmd};
    const vk::SubmitInfo2 submit{
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commands,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    if (d.queue.submit2(submit, done, dl) == vk::Result::eSuccess) {
        swapchain.Present(index);
    } else {
        // The renderer waits on done before its first frame, so it must still signal.
        (void)d.queue.submit2(vk::SubmitInfo2{}, done, dl);
    }
    (void)d.device.waitForFences(done, true, UINT64_MAX, dl);
    memory.Destroy(staging);
    memory.Destroy(picture);
    LOG_INFO(Render_Vulkan, "GR2port: splash {}x{} shown", extent.width, extent.height);
}

} // namespace Gr2Port::Splash
