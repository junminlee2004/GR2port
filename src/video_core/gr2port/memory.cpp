// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <string_view>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/gr2port/emulated_gpu.h"
#include "video_core/gr2port/memory.h"

// After vk_common.h, which sets the Vulkan header options.
#include <vk_mem_alloc.h>

namespace Gr2Port {

namespace {

// Device-local memory the renderer leaves free for the emulated GPU's renderer, which allocates
// more when the game opens its pause menu while it runs, and less while it is off.
constexpr u64 ForkHeadroom = 512_MB;
constexpr u64 OffHeadroom = 64_MB;
// Small blocks keep the memory the allocator holds close to what the renderer uses.
constexpr u64 BlockSize = 32_MB;
// Device-local allocations at least this large are logged with the code that made them.
constexpr u64 LoggedBytes = 8_MB;

void LogLarge(const std::source_location& where, u64 bytes, std::string_view what) {
    if (bytes < LoggedBytes) {
        return;
    }
    LOG_INFO(Render_Vulkan, "GR2port memory: {:.1f} MB for {} at {}:{}",
             static_cast<double>(bytes) / (1 << 20), what,
             std::filesystem::path{where.file_name()}.filename().string(), where.line());
}

} // namespace

bool Memory::Create() {
    const VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr = d.dispatch.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = d.dispatch.vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo info{
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
                 (d.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0u),
        .physicalDevice = d.gpu,
        .device = d.device,
        .preferredLargeHeapBlockSize = BlockSize,
        .pVulkanFunctions = &functions,
        .instance = d.instance,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    if (vmaCreateAllocator(&info, &allocator) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2port: no memory allocator");
        return false;
    }
    if (const char* forced = std::getenv("GR2PORT_VRAM_MB"); forced != nullptr) {
        const u64 megabytes = std::strtoull(forced, nullptr, 10);
        if (megabytes != 0) {
            forced_budget = megabytes << 20;
            LOG_INFO(Render_Vulkan, "GR2port memory: device-local budget capped at {} MB",
                     megabytes);
        }
    }
    return true;
}

Buffer Memory::CreateBuffer(u64 size, vk::BufferUsageFlags usage, Placement placement,
                            std::source_location where) {
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage =
            static_cast<VkBufferUsageFlags>(usage | vk::BufferUsageFlagBits::eShaderDeviceAddress),
    };
    VmaAllocationCreateInfo allocation_info{.usage = VMA_MEMORY_USAGE_AUTO};
    if (placement == Placement::Staging) {
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else if (placement == Placement::Upload) {
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    } else if (placement == Placement::Readback) {
        allocation_info.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        allocation_info.requiredFlags =
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    Buffer buffer{.size = size};
    VkBuffer handle = VK_NULL_HANDLE;
    VmaAllocationInfo result{};
    if (vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &handle, &buffer.allocation,
                        &result) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2port: no memory for a buffer of {} bytes", size);
        return {};
    }
    buffer.buffer = handle;
    if (placement == Placement::Device) {
        LogLarge(where, result.size, "a buffer");
    }
    buffer.mapped = static_cast<u8*>(result.pMappedData);
    buffer.address = d.device.getBufferAddress({.buffer = buffer.buffer}, d.dispatch);
    return buffer;
}

void Memory::Refresh(u64 frame) {
    vmaSetCurrentFrameIndex(allocator, static_cast<u32>(frame));
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(allocator, &properties);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(allocator, budgets.data());
    device_budget = {};
    for (u32 i = 0; i < properties->memoryHeapCount; i++) {
        if ((properties->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) {
            continue;
        }
        device_budget.usage += budgets[i].usage;
        device_budget.budget += budgets[i].budget;
        device_budget.own += budgets[i].statistics.blockBytes;
    }
    device_budget.budget = std::min(device_budget.budget, forced_budget);
    reserved = 0;
}

u64 Memory::Headroom() const {
    return EmulatedGpu::On() ? ForkHeadroom : OffHeadroom;
}

bool Memory::Reserve(u64 bytes) {
    if (device_budget.usage + reserved + bytes + Headroom() > device_budget.budget) {
        if (!logged_full) {
            logged_full = true;
            LOG_WARNING(Render_Vulkan,
                        "GR2port: device memory is {} of {} MB; uploads wait for room",
                        device_budget.usage >> 20, device_budget.budget >> 20);
        }
        return false;
    }
    reserved += bytes;
    return true;
}

void Memory::Destroy(Buffer& buffer) {
    if (buffer.allocation != nullptr) {
        vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
    }
    buffer = {};
}

Image Memory::CreateImage(vk::Format format, vk::Extent2D extent, vk::ImageUsageFlags usage,
                          vk::ImageAspectFlags aspect, u32 layers, u32 levels,
                          vk::ImageCreateFlags flags, std::source_location where) {
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = static_cast<VkImageCreateFlags>(flags),
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {extent.width, extent.height, 1},
        .mipLevels = levels,
        .arrayLayers = layers,
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
    VmaAllocationInfo result{};
    if (vmaCreateImage(allocator, &image_info, &allocation_info, &handle, &image.allocation,
                       &result) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2port: no memory for a {}x{} {} image", extent.width,
                  extent.height, vk::to_string(format));
        return {};
    }
    image.image = handle;
    image.size = result.size;
    LogLarge(where, image.size,
             fmt::format("a {}x{} {} image", extent.width, extent.height, vk::to_string(format)));
    const vk::ImageViewCreateInfo view_info{
        .image = image.image,
        .viewType = layers > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, levels, 0, layers},
    };
    image.view = d.device.createImageView(view_info, nullptr, d.dispatch).value;
    return image;
}

Image Memory::CreateVolume(vk::Format format, vk::Extent3D extent, vk::ImageUsageFlags usage,
                           std::source_location where) {
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_3D,
        .format = static_cast<VkFormat>(format),
        .extent = {extent.width, extent.height, extent.depth},
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
    Image image{.format = format, .extent = {extent.width, extent.height}};
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocationInfo result{};
    if (vmaCreateImage(allocator, &image_info, &allocation_info, &handle, &image.allocation,
                       &result) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2port: no memory for a {}x{}x{} {} volume", extent.width,
                  extent.height, extent.depth, vk::to_string(format));
        return {};
    }
    image.image = handle;
    image.size = result.size;
    LogLarge(where, image.size,
             fmt::format("a {}x{}x{} {} volume", extent.width, extent.height, extent.depth,
                         vk::to_string(format)));
    const vk::ImageViewCreateInfo view_info{
        .image = image.image,
        .viewType = vk::ImageViewType::e3D,
        .format = format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    image.view = d.device.createImageView(view_info, nullptr, d.dispatch).value;
    return image;
}

Image Memory::CreateTexture(vk::Format format, vk::Extent2D extent, u32 levels,
                            vk::ComponentMapping mapping, bool cube, bool dedicated) {
    const u32 layers = cube ? 6 : 1;
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : VkImageCreateFlags{0},
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {extent.width, extent.height, 1},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    // Thousands of small textures share the allocator's blocks.
    const VmaAllocationCreateInfo allocation_info{
        .flags = dedicated ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0u,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    Image image{.format = format, .extent = extent};
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocationInfo result{};
    if (vmaCreateImage(allocator, &image_info, &allocation_info, &handle, &image.allocation,
                       &result) != VK_SUCCESS) {
        return {};
    }
    image.image = handle;
    image.size = result.size;
    const vk::ImageViewCreateInfo view_info{
        .image = image.image,
        .viewType = cube ? vk::ImageViewType::eCube : vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, levels, 0, layers},
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

} // namespace Gr2Port
