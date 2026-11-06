// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include <SDL3/SDL_vulkan.h>

#include "common/logging/log.h"
#include "video_core/gr2port/device.h"

namespace Gr2Port {

namespace {

constexpr u32 ApiVersion = VK_API_VERSION_1_3;
constexpr const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";

// The validation layer checks the renderer's own instance whenever the loader finds it, except in
// timing runs, which set GR2PORT_NO_VALIDATION.
bool HasValidation(const Device& d) {
    if (std::getenv("GR2PORT_NO_VALIDATION") != nullptr) {
        return false;
    }
    const auto [result, layers] = vk::enumerateInstanceLayerProperties(d.dispatch);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    return std::ranges::any_of(layers, [](const vk::LayerProperties& layer) {
        return std::strcmp(layer.layerName, ValidationLayer) == 0;
    });
}

bool CreateInstance(Device& d) {
    const auto get_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (get_proc == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port: no Vulkan loader: {}", SDL_GetError());
        return false;
    }
    d.dispatch.init(get_proc);
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port: no surface extensions: {}", SDL_GetError());
        return false;
    }
    const vk::ApplicationInfo application{
        .pApplicationName = "GR2port",
        .apiVersion = ApiVersion,
    };
    const bool validation = HasValidation(d);
    const vk::InstanceCreateInfo info{
        .pApplicationInfo = &application,
        .enabledLayerCount = validation ? 1u : 0u,
        .ppEnabledLayerNames = &ValidationLayer,
        .enabledExtensionCount = count,
        .ppEnabledExtensionNames = extensions,
    };
    const auto [result, instance] = vk::createInstance(info, nullptr, d.dispatch);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: no Vulkan instance: {}", vk::to_string(result));
        return false;
    }
    d.instance = instance;
    d.dispatch.init(instance);
    if (validation) {
        LOG_INFO(Render_Vulkan, "GR2port: the validation layer checks the renderer");
    }
    return true;
}

std::optional<u32> PresentFamily(const Device& d, vk::PhysicalDevice gpu) {
    const auto families = gpu.getQueueFamilyProperties(d.dispatch);
    for (u32 i = 0; i < families.size(); i++) {
        if (!(families[i].queueFlags & vk::QueueFlagBits::eGraphics)) {
            continue;
        }
        const auto [result, present] = gpu.getSurfaceSupportKHR(i, d.surface, d.dispatch);
        if (result == vk::Result::eSuccess && present) {
            return i;
        }
    }
    return std::nullopt;
}

bool PickGpu(Device& d) {
    const auto [result, gpus] = d.instance.enumeratePhysicalDevices(d.dispatch);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    bool found = false;
    for (const vk::PhysicalDevice gpu : gpus) {
        const auto properties = gpu.getProperties(d.dispatch);
        const std::optional<u32> family = PresentFamily(d, gpu);
        if (properties.apiVersion < ApiVersion || !family) {
            continue;
        }
        const bool discrete = properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
        if (!found || discrete) {
            d.gpu = gpu;
            d.queue_family = *family;
            found = true;
        }
        if (discrete) {
            break;
        }
    }
    return found;
}

} // namespace

bool CreateDevice(SDL_Window* window, Device& d) {
    if (!CreateInstance(d)) {
        return false;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, d.instance, nullptr, &surface)) {
        LOG_ERROR(Render_Vulkan, "GR2port: no surface: {}", SDL_GetError());
        return false;
    }
    d.surface = surface;
    if (!PickGpu(d)) {
        LOG_ERROR(Render_Vulkan, "GR2port: no Vulkan 1.3 GPU presents to the window");
        return false;
    }

    vk::PhysicalDeviceVulkan13Features features13{
        .synchronization2 = true,
        .dynamicRendering = true,
    };
    vk::PhysicalDeviceVulkan12Features features12{
        .pNext = &features13,
        .storageBuffer8BitAccess = true,
        .shaderSampledImageArrayNonUniformIndexing = true,
        .descriptorBindingSampledImageUpdateAfterBind = true,
        .descriptorBindingUpdateUnusedWhilePending = true,
        .descriptorBindingPartiallyBound = true,
        .runtimeDescriptorArray = true,
        .scalarBlockLayout = true,
        .timelineSemaphore = true,
        .bufferDeviceAddress = true,
    };
    vk::PhysicalDeviceVulkan11Features features11{
        .pNext = &features12,
        .storageBuffer16BitAccess = true,
        .shaderDrawParameters = true,
    };
    const vk::PhysicalDeviceFeatures supported = d.gpu.getFeatures(d.dispatch);
    d.geometry_shaders = supported.geometryShader != VK_FALSE;
    d.tessellation_shaders = supported.tessellationShader != VK_FALSE;
    d.storage_extended_formats = supported.shaderStorageImageExtendedFormats != VK_FALSE;
    vk::PhysicalDeviceFeatures2 features{
        .pNext = &features11,
        .features = {.geometryShader = d.geometry_shaders,
                     .tessellationShader = d.tessellation_shaders,
                     .multiDrawIndirect = true,
                     .drawIndirectFirstInstance = true,
                     .samplerAnisotropy = true,
                     .textureCompressionBC = true,
                     .shaderStorageImageExtendedFormats = d.storage_extended_formats,
                     .shaderInt64 = true},
    };
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = d.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    // Push descriptors bind the buffers of the texture detile shaders.
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    const auto [extensions_result, available] =
        d.gpu.enumerateDeviceExtensionProperties(nullptr, d.dispatch);
    for (const vk::ExtensionProperties& extension : available) {
        if (std::string_view{extension.extensionName.data()} ==
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) {
            d.memory_budget = true;
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
    }
    const vk::DeviceCreateInfo info{
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    const auto [result, device] = d.gpu.createDevice(info, nullptr, d.dispatch);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: no device: {}", vk::to_string(result));
        return false;
    }
    d.device = device;
    d.dispatch.init(device);
    d.queue = device.getQueue(d.queue_family, 0, d.dispatch);

    const auto properties = d.gpu.getProperties(d.dispatch);
    const auto families = d.gpu.getQueueFamilyProperties(d.dispatch);
    if (families[d.queue_family].timestampValidBits != 0) {
        d.timestamp_period = properties.limits.timestampPeriod;
    }
    LOG_INFO(Render_Vulkan, "GR2port: renderer device {} (queue family {})",
             properties.deviceName.data(), d.queue_family);
    return true;
}

} // namespace Gr2Port
