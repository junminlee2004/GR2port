// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "video_core/gr2_pt/memory.h"

// After vk_common.h, which sets the Vulkan header options.
#include <vk_mem_alloc.h>

namespace Gr2Pt {

bool Memory::Create() {
    const VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr = d.dispatch.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = d.dispatch.vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo info{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = d.gpu,
        .device = d.device,
        .pVulkanFunctions = &functions,
        .instance = d.instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    if (vmaCreateAllocator(&info, &allocator) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no memory allocator");
        return false;
    }
    return true;
}

Buffer Memory::CreateBuffer(u64 size, vk::BufferUsageFlags usage, Placement placement) {
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage =
            static_cast<VkBufferUsageFlags>(usage | vk::BufferUsageFlagBits::eShaderDeviceAddress),
    };
    VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
    if (placement == Placement::Upload) {
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    Buffer buffer{.size = size};
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &handle, &buffer.allocation,
                        &result) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no memory for a buffer of {} bytes", size);
        return {};
    }
    buffer.buffer = handle;
    buffer.mapped = static_cast<u8*>(result.pMappedData);
    buffer.address = d.device.getBufferAddress({.buffer = buffer.buffer}, d.dispatch);
    return buffer;
}

void Memory::Destroy(Buffer& buffer) {
    if (buffer.allocation != nullptr) {
        vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

Image Memory::CreateImage(vk::Format format, vk::Extent2D extent, vk::ImageUsageFlags usage,
                          vk::ImageAspectFlags aspect) {
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = static_cast<VkImageUsageFlags>(usage),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo allocation_info{
        .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    Image image{.format = format, .extent = extent};
    VkImage handle = VK_NULL_HANDLE;
    if (vmaCreateImage(allocator, &image_info, &allocation_info, &handle, &image.allocation,
                       nullptr) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no memory for a {}x{} {} image", extent.width,
                  extent.height, vk::to_string(format));
        return {};
    }
    image.image = handle;
    const vk::ImageViewCreateInfo view_info{
        .image = image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    };
    image.view = d.device.createImageView(view_info, nullptr, d.dispatch).value;
    return image;
}

void Memory::Destroy(Image& image) {
    if (image.view) {
        d.device.destroyImageView(image.view, nullptr, d.dispatch);
    }
    if (image.allocation != nullptr) {
        vmaDestroyImage(allocator, image.image, image.allocation);
    }
    image = {};
}

} // namespace Gr2Pt
