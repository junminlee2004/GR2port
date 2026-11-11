// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <SDL3/SDL_vulkan.h>
#include <unistd.h>

#include "launcher/qt/video_card.h"

#include "graphics/renderer_vulkan/vk_common.h"

namespace Gr2Port::Launcher {

namespace {

// The memory heaps on the GPU count as its own, the others as lent RAM.
VideoCard Describe(vk::PhysicalDevice gpu, const vk::detail::DispatchLoaderDynamic& dispatch) {
    const auto properties = gpu.getProperties(dispatch);
    const auto memory = gpu.getMemoryProperties(dispatch);
    VideoCard card{.name = properties.deviceName.data(),
                   .integrated = properties.deviceType == vk::PhysicalDeviceType::eIntegratedGpu};
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        const vk::MemoryHeap& heap = memory.memoryHeaps[i];
        (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal ? card.bytes : card.shared) += heap.size;
    }
    return card;
}

VideoCard Find(PFN_vkGetInstanceProcAddr get_proc) {
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(get_proc);
    const vk::ApplicationInfo application{
        .pApplicationName = "GR2port",
        .apiVersion = VK_API_VERSION_1_3,
    };
    const vk::InstanceCreateInfo info{.pApplicationInfo = &application};
    const auto [result, instance] = vk::createInstance(info, nullptr, dispatch);
    if (result != vk::Result::eSuccess) {
        return {};
    }
    dispatch.init(instance);
    std::optional<VideoCard> first;
    std::optional<VideoCard> discrete;
    const auto [listed, gpus] = instance.enumeratePhysicalDevices(dispatch);
    if (listed == vk::Result::eSuccess) {
        for (const vk::PhysicalDevice gpu : gpus) {
            if (gpu.getProperties(dispatch).deviceType == vk::PhysicalDeviceType::eCpu) {
                continue;
            }
            const VideoCard card = Describe(gpu, dispatch);
            if (!first) {
                first = card;
            }
            if (!card.integrated) {
                discrete = card;
                break;
            }
        }
    }
    instance.destroy(nullptr, dispatch);
    return discrete.value_or(first.value_or(VideoCard{}));
}

} // namespace

VideoCard FindVideoCard() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        return {};
    }
    const auto get_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    const VideoCard card = get_proc != nullptr ? Find(get_proc) : VideoCard{};
    SDL_Vulkan_UnloadLibrary();
    return card;
}

u64 InstalledRam() {
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page_size > 0 ? static_cast<u64>(pages) * static_cast<u64>(page_size) : 0;
}

} // namespace Gr2Port::Launcher
