// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_vulkan.h>

#include "common/logging/log.h"
#include "graphics/gr2port/device.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/upscaler.h"

namespace Gr2Port {

namespace {

constexpr u32 ApiVersion = VK_API_VERSION_1_3;
constexpr const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";

template <typename Features>
struct Need {
    const char* name;
    vk::Bool32 Features::* member;
};

using Features10 = vk::PhysicalDeviceFeatures;
using Features11 = vk::PhysicalDeviceVulkan11Features;
using Features12 = vk::PhysicalDeviceVulkan12Features;
using Features13 = vk::PhysicalDeviceVulkan13Features;

// The features the renderer cannot run without, each used by a SPIR-V capability of its shaders
// or by its commands. The device enables these, and a GPU without one of them is not picked.
constexpr std::array Needs10{
    Need<Features10>{"multiDrawIndirect", &Features10::multiDrawIndirect},
    Need<Features10>{"drawIndirectFirstInstance", &Features10::drawIndirectFirstInstance},
    Need<Features10>{"samplerAnisotropy", &Features10::samplerAnisotropy},
    Need<Features10>{"textureCompressionBC", &Features10::textureCompressionBC},
    Need<Features10>{"shaderInt64", &Features10::shaderInt64},
};
constexpr std::array Needs11{
    Need<Features11>{"shaderDrawParameters", &Features11::shaderDrawParameters},
    Need<Features11>{"storageBuffer16BitAccess", &Features11::storageBuffer16BitAccess},
};
constexpr std::array Needs12{
    Need<Features12>{"shaderSampledImageArrayNonUniformIndexing",
                     &Features12::shaderSampledImageArrayNonUniformIndexing},
    Need<Features12>{"descriptorBindingSampledImageUpdateAfterBind",
                     &Features12::descriptorBindingSampledImageUpdateAfterBind},
    Need<Features12>{"descriptorBindingUpdateUnusedWhilePending",
                     &Features12::descriptorBindingUpdateUnusedWhilePending},
    Need<Features12>{"descriptorBindingPartiallyBound",
                     &Features12::descriptorBindingPartiallyBound},
    Need<Features12>{"runtimeDescriptorArray", &Features12::runtimeDescriptorArray},
    Need<Features12>{"bufferDeviceAddress", &Features12::bufferDeviceAddress},
    Need<Features12>{"storageBuffer8BitAccess", &Features12::storageBuffer8BitAccess},
    Need<Features12>{"uniformBufferStandardLayout", &Features12::uniformBufferStandardLayout},
};
constexpr std::array Needs13{
    Need<Features13>{"synchronization2", &Features13::synchronization2},
    Need<Features13>{"dynamicRendering", &Features13::dynamicRendering},
};
// Push descriptors bind the buffers of the texture detile shaders.
constexpr std::array NeededExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                      VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};

template <typename Features, std::size_t Count>
void Enable(Features& features, const std::array<Need<Features>, Count>& needs) {
    for (const Need<Features>& need : needs) {
        features.*need.member = VK_TRUE;
    }
}

void Append(std::string& list, std::string_view name) {
    if (!list.empty()) {
        list += ", ";
    }
    list += name;
}

template <typename Features, std::size_t Count>
void AppendMissing(std::string& list, const Features& supported,
                   const std::array<Need<Features>, Count>& needs) {
    for (const Need<Features>& need : needs) {
        if (supported.*need.member == VK_FALSE) {
            Append(list, need.name);
        }
    }
}

// The needed features and extensions the GPU lacks, or empty when it has them all.
std::string Missing(const Device& d, vk::PhysicalDevice gpu) {
    const auto chain =
        gpu.getFeatures2<vk::PhysicalDeviceFeatures2, Features11, Features12, Features13>(
            d.dispatch);
    std::string missing;
    AppendMissing(missing, chain.get<vk::PhysicalDeviceFeatures2>().features, Needs10);
    AppendMissing(missing, chain.get<Features11>(), Needs11);
    AppendMissing(missing, chain.get<Features12>(), Needs12);
    AppendMissing(missing, chain.get<Features13>(), Needs13);
    const auto [result, available] = gpu.enumerateDeviceExtensionProperties(nullptr, d.dispatch);
    for (const char* extension : NeededExtensions) {
        const bool found =
            std::ranges::any_of(available, [extension](const vk::ExtensionProperties& property) {
                return std::string_view{property.extensionName.data()} == extension;
            });
        if (!found) {
            Append(missing, extension);
        }
    }
    return missing;
}

// The validation layer checks the renderer's own instance only in runs that set
// GR2PORT_VALIDATION, and only when the loader finds it.
bool HasValidation(const Device& d) {
    if (std::getenv("GR2PORT_VALIDATION") == nullptr) {
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
    const char* const* surface_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (surface_extensions == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port: no surface extensions: {}", SDL_GetError());
        return false;
    }
    std::vector<const char*> extensions{surface_extensions, surface_extensions + count};
    Upscaler::ExtendInstance(extensions, d.dispatch);
    const vk::ApplicationInfo application{
        .pApplicationName = "GR2port",
        .apiVersion = ApiVersion,
    };
    const bool validation = HasValidation(d);
    const vk::InstanceCreateInfo info{
        .pApplicationInfo = &application,
        .enabledLayerCount = validation ? 1u : 0u,
        .ppEnabledLayerNames = &ValidationLayer,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
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
        const std::string_view name{properties.deviceName.data()};
        if (properties.apiVersion < ApiVersion) {
            LOG_INFO(Render_Vulkan, "GR2port: {} has Vulkan {}.{}; the renderer needs 1.3", name,
                     VK_API_VERSION_MAJOR(properties.apiVersion),
                     VK_API_VERSION_MINOR(properties.apiVersion));
            continue;
        }
        const std::optional<u32> family = PresentFamily(d, gpu);
        if (!family) {
            LOG_INFO(Render_Vulkan, "GR2port: {} cannot present to the window", name);
            continue;
        }
        if (const std::string missing = Missing(d, gpu); !missing.empty()) {
            LOG_INFO(Render_Vulkan, "GR2port: {} lacks {}", name, missing);
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
        LOG_ERROR(Render_Vulkan, "GR2port: no GPU has what the renderer needs");
        return false;
    }

    vk::PhysicalDeviceVulkan13Features features13;
    vk::PhysicalDeviceVulkan12Features features12{.pNext = &features13};
    vk::PhysicalDeviceVulkan11Features features11{.pNext = &features12};
    vk::PhysicalDeviceFeatures2 features{.pNext = &features11};
    Enable(features.features, Needs10);
    Enable(features11, Needs11);
    Enable(features12, Needs12);
    Enable(features13, Needs13);
    const vk::PhysicalDeviceFeatures supported = d.gpu.getFeatures(d.dispatch);
    d.geometry_shaders = supported.geometryShader != VK_FALSE;
    d.tessellation_shaders = supported.tessellationShader != VK_FALSE;
    d.storage_extended_formats = supported.shaderStorageImageExtendedFormats != VK_FALSE;
    features.features.geometryShader = d.geometry_shaders;
    features.features.tessellationShader = d.tessellation_shaders;
    features.features.shaderStorageImageExtendedFormats = d.storage_extended_formats;
    const float priority = 1.0f;
    const vk::DeviceQueueCreateInfo queue_info{
        .queueFamilyIndex = d.queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    std::vector<const char*> extensions{NeededExtensions.begin(), NeededExtensions.end()};
    const auto [extensions_result, available] =
        d.gpu.enumerateDeviceExtensionProperties(nullptr, d.dispatch);
    vk::PhysicalDeviceFaultFeaturesEXT fault_features;
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT blend_features{
        .extendedDynamicState3ColorBlendEnable = true,
        .extendedDynamicState3ColorBlendEquation = true,
    };
    for (const vk::ExtensionProperties& extension : available) {
        const std::string_view name{extension.extensionName.data()};
        if (name == VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) {
            d.memory_budget = true;
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        } else if (name == VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) {
            d.checkpoints = true;
            extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        } else if (name == VK_EXT_DEVICE_FAULT_EXTENSION_NAME) {
            vk::PhysicalDeviceFeatures2 query{.pNext = &fault_features};
            d.gpu.getFeatures2(&query, d.dispatch);
            d.device_fault = fault_features.deviceFault != VK_FALSE;
            if (d.device_fault) {
                extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            }
        } else if (name == VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) {
            vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT supported_blend;
            vk::PhysicalDeviceFeatures2 query{.pNext = &supported_blend};
            d.gpu.getFeatures2(&query, d.dispatch);
            d.dynamic_blend = supported_blend.extendedDynamicState3ColorBlendEnable != VK_FALSE &&
                              supported_blend.extendedDynamicState3ColorBlendEquation != VK_FALSE;
            if (d.dynamic_blend) {
                extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
            }
        }
    }
    if (d.dynamic_blend) {
        blend_features.pNext = features.pNext;
        features.pNext = &blend_features;
    }
    if (d.device_fault) {
        fault_features.deviceFaultVendorBinary = false;
        fault_features.pNext = features.pNext;
        features.pNext = &fault_features;
    }
    vk::DeviceCreateInfo info{
        .pNext = &features,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = static_cast<u32>(extensions.size()),
        .ppEnabledExtensionNames = extensions.data(),
    };
    Upscaler::ExtendDevice(d.gpu, d.dispatch, info, features, features11, features12, features13);
    const auto [result, device] = d.gpu.createDevice(info, nullptr, d.dispatch);
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2port: no device: {}", vk::to_string(result));
        return false;
    }
    d.device = device;
    d.dispatch.init(device);
    Upscaler::OnDeviceCreated(d);
    d.queue = device.getQueue(d.queue_family, 0, d.dispatch);

    const auto properties = d.gpu.getProperties(d.dispatch);
    const auto families = d.gpu.getQueueFamilyProperties(d.dispatch);
    if (families[d.queue_family].timestampValidBits != 0) {
        d.timestamp_period = properties.limits.timestampPeriod;
    }
    const auto driver =
        d.gpu
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>(
                d.dispatch)
            .get<vk::PhysicalDeviceDriverProperties>();
    LOG_INFO(Render_Vulkan,
             "GR2port: renderer device {} (queue family {}), checkpoints {}, fault information {}, "
             "dynamic blend {}, driver {} {}",
             properties.deviceName.data(), d.queue_family, d.checkpoints, d.device_fault,
             d.dynamic_blend, driver.driverName.data(), driver.driverInfo.data());
    LoadPipelineCache(d);
    return true;
}

} // namespace Gr2Port
