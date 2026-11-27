// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
// After windows.h.
#include <psapi.h>
#endif

#include "common/logging/log.h"
#include "common/path_util.h"
#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/catcher.h"
#include "graphics/gr2port/catcher_crash.h"
#include "graphics/gr2port/catcher_marks.h"
#include "graphics/gr2port/catcher_note.h"
#include "graphics/gr2port/catcher_outside.h"
#include "graphics/gr2port/catcher_stacks.h"
#include "graphics/gr2port/catcher_system.h"
#include "graphics/gr2port/catcher_test.h"
#include "graphics/gr2port/device_lost.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pass_times.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/settings.h"

namespace Gr2Port::Catcher {

namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 Calls = static_cast<u32>(Call::Count);
constexpr std::array<const char*, Calls> CallNames{
    "waiting for the game's frame",
    "the wait for an earlier frame",
    "the swapchain image's acquire",
    "recording the frame",
    "the frame's submit",
    "the present",
};
constexpr std::array<const char*, Calls> CallColumns{
    "idle", "wait", "acquire", "record", "submit", "present",
};

constexpr u32 Passes = static_cast<u32>(PassTimes::Pass::Count);
constexpr u32 TracePasses = static_cast<u32>(DeviceLost::Pass::Count);
// The frames whose times a lost device's report lists, and those listed with every pass.
constexpr u32 KeptFrames = 256;
constexpr u32 FullFrames = 6;
// The passes a kept frame's line names: its longest.
constexpr u32 NamedPasses = 6;
// The driver's messages and the watch's lines logged at most.
constexpr u32 MaxMessages = 400;
constexpr u32 MaxWatchLines = 600;
// The watch looks this often, at a frame the GPU has had for FirstLook, and logs a frame the GPU
// has had for FirstStall, then at each doubling.
constexpr std::chrono::milliseconds Tick{50};
constexpr std::chrono::milliseconds FirstLook{100};
constexpr std::chrono::milliseconds FirstStall{250};
// The render thread in one call, and the game without a frame, are logged from these on.
constexpr std::chrono::milliseconds FirstBlocked{1000};
constexpr std::chrono::milliseconds FirstSilence{3000};
constexpr u32 FrameSlots = 2;
// The watch looks closer at a frame the GPU has had for CloserLook, and again at CloserLookAgain;
// at a render thread that long in one call; and at a game without frames once it has drawn
// BootFrames. A run takes MaxCloserLooks of them, of MaxCloserLines lines in all.
constexpr std::chrono::milliseconds CloserLook{1000};
constexpr std::chrono::milliseconds CloserLookAgain{8000};
constexpr u64 BootFrames = 600;
constexpr u32 MaxCloserLooks = 6;
constexpr u32 MaxCloserLines = 4000;
// The thread that watches the watch looks this often, and speaks when the watch has not run for
// GuardFirst, then at each doubling.
constexpr std::chrono::milliseconds GuardTick{500};
constexpr std::chrono::milliseconds GuardFirst{6000};

// The environment variables that change which driver, layers or window system the process gets.
constexpr std::array EnvironmentNames{
    "VK_ICD_FILENAMES",
    "VK_DRIVER_FILES",
    "VK_ADD_DRIVER_FILES",
    "VK_INSTANCE_LAYERS",
    "VK_LOADER_LAYERS_ENABLE",
    "VK_LOADER_LAYERS_DISABLE",
    "VK_LAYER_PATH",
    "VK_ADD_LAYER_PATH",
    "VK_LOADER_DEBUG",
    "DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1",
    "DISABLE_LAYER_NV_OPTIMUS_1",
    "__NV_PRIME_RENDER_OFFLOAD",
    "__VK_LAYER_NV_optimus",
    "__GLX_VENDOR_LIBRARY_NAME",
    "__GL_THREADED_OPTIMIZATIONS",
    "DRI_PRIME",
    "MESA_LOADER_DRIVER_OVERRIDE",
    "MESA_VK_DEVICE_SELECT",
    "MESA_VK_VERSION_OVERRIDE",
    "MESA_VK_WSI_PRESENT_MODE",
    "MESA_SHADER_CACHE_DISABLE",
    "ANV_DEBUG",
    "INTEL_DEBUG",
    "RADV_DEBUG",
    "RADV_PERFTEST",
    "AMD_VULKAN_ICD",
    "NVK_DEBUG",
    "MANGOHUD",
    "MANGOHUD_CONFIG",
    "ENABLE_VKBASALT",
    "ENABLE_GAMESCOPE_WSI",
    "GAMESCOPE_WAYLAND_DISPLAY",
    "OBS_VKCAPTURE",
    "LD_PRELOAD",
    "LD_LIBRARY_PATH",
    "SDL_VIDEODRIVER",
    "SDL_VIDEO_DRIVER",
    "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP",
    "WAYLAND_DISPLAY",
    "DISPLAY",
    "APPIMAGE",
    "SteamAppId",
    "SteamDeck",
    "GAMEMODERUNEXEC",
    "GR2PORT_VALIDATION",
    "GR2PORT_GPU_TRACE",
    "GR2PORT_CHECK_ADDRESSES",
};

struct Kept {
    u64 frame = 0;
    u64 game_frame = 0;
    u64 indices = 0;
    u32 draws = 0;
    u32 casters = 0;
    u32 particle_draws = 0;
    u32 samples = 0;
    u32 effect_draws = 0;
    u32 used_mb = 0;
    u32 budget_mb = 0;
    bool world = false;
    bool submitted = false;
    bool timed = false;
    std::array<float, Calls> cpu_ms{};
    std::array<float, Passes + 1> gpu_ms{};
};

Memory* g_memory = nullptr;
VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;
std::atomic<u32> g_messages{0};
std::vector<std::string> g_modules;

// The render thread's own.
std::array<Kept, KeptFrames> g_kept{};
u64 g_kept_count = 0;
Kept g_current{};
Clock::time_point g_since;

// What the watch reads.
std::atomic<u32> g_call{0};
std::atomic<s64> g_call_since{0};
std::atomic<u64> g_submits{0};
std::atomic<s64> g_submit_time{0};
std::atomic<u32> g_submit_slot{0};
std::atomic<bool> g_lost{false};
// The watch's last lines about a frame the GPU did not finish, for a lost device's report.
std::mutex g_watch_mutex;
std::vector<std::string> g_watch_lines;
std::atomic<u32> g_watch_count{0};
std::thread g_watch;
// When the watch last ran, for the thread that watches it.
std::atomic<s64> g_watch_beat{0};
std::thread g_guard;
std::atomic<u32> g_closer_looks{0};
// The history's entries, and the lines a report prints of it.
constexpr size_t Events = 256;
constexpr size_t EventChars = 64;
struct Event {
    Clock::time_point time;
    u64 game_frame = 0;
    u64 value = 0;
    std::array<char, EventChars> what{};
};
std::mutex g_events_mutex;
std::array<Event, Events> g_events{};
size_t g_event_next = 0;
size_t g_event_count = 0;
// The device, for the probe.
const Device* g_device = nullptr;
// When the process began, for the times on the catcher's lines.
const Clock::time_point g_began = Clock::now();
// The times the driver answered the render thread's wait with "not yet" since the wait began.
std::atomic<u32> g_wait_answers{0};
std::atomic<u32> g_closer_lines{0};

s64 Ticks(Clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
}

double MsSince(s64 ticks) {
    return static_cast<double>(Ticks(Clock::now()) - ticks) * 1e-3;
}

float Megabytes(u64 bytes) {
    return static_cast<float>(static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::string Version(u32 version) {
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

std::string Hex(const void* data, size_t bytes) {
    std::string text;
    text.reserve(bytes * 2);
    const auto* at = static_cast<const u8*>(data);
    for (size_t i = 0; i < bytes; i++) {
        text += fmt::format("{:02x}", at[i]);
    }
    return text;
}

// A features structure's switches, in the order the Vulkan header declares them: 1 for on.
std::string Switches(const void* data, size_t bytes, size_t skipped) {
    std::string text;
    const auto* at = static_cast<const u8*>(data) + skipped;
    for (size_t i = 0; i + sizeof(VkBool32) <= bytes - skipped; i += sizeof(VkBool32)) {
        VkBool32 value;
        std::memcpy(&value, at + i, sizeof(value));
        text += value != VK_FALSE ? '1' : '0';
    }
    return text;
}

// Logs a long list as lines of a readable length.
void LogList(std::string_view title, const std::vector<std::string>& items) {
    constexpr size_t LineLength = 400;
    std::string line;
    size_t part = 0;
    const auto flush = [&] {
        LOG_INFO(Render_Vulkan, "GR2port catcher: {} ({} in all, part {}): {}", title, items.size(),
                 ++part, line);
        line.clear();
    };
    for (const std::string& item : items) {
        if (!line.empty() && line.size() + item.size() > LineLength) {
            flush();
        }
        line += line.empty() ? "" : ", ";
        line += item;
    }
    if (!line.empty() || items.empty()) {
        flush();
    }
}

// The files the process has loaded: the game's own, the drivers', and what overlays, recorders
// and other tools put into it.
std::vector<std::string> Modules() {
    std::vector<std::string> modules;
#ifdef _WIN32
    std::vector<HMODULE> handles(2048);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), handles.data(),
                            static_cast<DWORD>(handles.size() * sizeof(HMODULE)), &needed)) {
        return modules;
    }
    handles.resize(std::min<size_t>(handles.size(), needed / sizeof(HMODULE)));
    for (const HMODULE handle : handles) {
        std::array<wchar_t, 1024> wide{};
        const DWORD length =
            GetModuleFileNameW(handle, wide.data(), static_cast<DWORD>(wide.size()));
        if (length == 0) {
            continue;
        }
        std::array<char, 3072> narrow{};
        const int bytes =
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(length), narrow.data(),
                                static_cast<int>(narrow.size()), nullptr, nullptr);
        if (bytes > 0) {
            modules.emplace_back(narrow.data(), static_cast<size_t>(bytes));
        }
    }
#else
    std::ifstream maps{"/proc/self/maps"};
    std::set<std::string> seen;
    std::string line;
    while (std::getline(maps, line)) {
        const size_t at = line.find('/');
        if (at == std::string::npos || line.find(".so", at) == std::string::npos) {
            continue;
        }
        if (seen.insert(line.substr(at)).second) {
            modules.push_back(line.substr(at));
        }
    }
#endif
    return modules;
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const u32 number = g_messages.fetch_add(1) + 1;
    if (number > MaxMessages || data == nullptr) {
        return VK_FALSE;
    }
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool warning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
    const std::string text = fmt::format(
        "GR2port catcher: driver message {} ({}, type {:#x}, {}): {}", number,
        error     ? "error"
        : warning ? "warning"
                  : "note",
        static_cast<u32>(types), data->pMessageIdName != nullptr ? data->pMessageIdName : "no name",
        data->pMessage != nullptr ? data->pMessage : "");
    if (error) {
        LOG_ERROR(Render_Vulkan, "{}", text);
        Common::Log::Flush();
    } else {
        LOG_INFO(Render_Vulkan, "{}", text);
    }
    return VK_FALSE;
}

void LogSystem() {
    const char* const video = SDL_GetCurrentVideoDriver();
    int seconds = 0;
    int percent = 0;
    const SDL_PowerState power = SDL_GetPowerInfo(&seconds, &percent);
    LOG_INFO(Render_Vulkan,
             "GR2port catcher: window system {}, power state {} (0 error, 1 unknown, 2 on battery, "
             "3 no battery, 4 charging, 5 charged), battery {}%, {} MB of RAM, {} CPU threads",
             video != nullptr ? video : "none", static_cast<int>(power), percent,
             SDL_GetSystemRAM(), SDL_GetNumLogicalCPUCores());
    int count = 0;
    SDL_DisplayID* const displays = SDL_GetDisplays(&count);
    for (int i = 0; displays != nullptr && i < count; i++) {
        SDL_Rect area{};
        SDL_GetDisplayBounds(displays[i], &area);
        const SDL_DisplayMode* const mode = SDL_GetCurrentDisplayMode(displays[i]);
        const char* const name = SDL_GetDisplayName(displays[i]);
        LOG_INFO(Render_Vulkan,
                 "GR2port catcher: display {} \"{}\" at {},{} of {}x{}, mode {}x{} at {:.2f} Hz, "
                 "pixel density {:.2f}, content scale {:.2f}",
                 i, name != nullptr ? name : "", area.x, area.y, area.w, area.h,
                 mode != nullptr ? mode->w : 0, mode != nullptr ? mode->h : 0,
                 mode != nullptr ? mode->refresh_rate : 0.0f,
                 mode != nullptr ? mode->pixel_density : 0.0f,
                 SDL_GetDisplayContentScale(displays[i]));
    }
    SDL_free(displays);
    std::vector<std::string> environment;
    for (const char* name : EnvironmentNames) {
        if (const char* const value = std::getenv(name); value != nullptr) {
            environment.push_back(fmt::format("{}={}", name, value));
        }
    }
    LogList("environment variables set, of those that pick drivers, layers and windows",
            environment);
    LOG_INFO(Render_Vulkan, "GR2port catcher: settings {}", Settings::Describe());
    g_modules = Modules();
    LogList("files loaded into the process", g_modules);
}

std::string DescribeGpu(const Device& d, vk::PhysicalDevice gpu) {
    const auto chain =
        gpu.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>(
            d.dispatch);
    const auto& properties = chain.get<vk::PhysicalDeviceProperties2>().properties;
    const auto& driver = chain.get<vk::PhysicalDeviceDriverProperties>();
    return fmt::format("\"{}\", {}, vendor {:#06x} device {:#06x}, Vulkan {}, driver version "
                       "{:#x}, driver {} \"{}\" \"{}\", conformance {}.{}.{}.{}",
                       properties.deviceName.data(), vk::to_string(properties.deviceType),
                       properties.vendorID, properties.deviceID, Version(properties.apiVersion),
                       properties.driverVersion, vk::to_string(driver.driverID),
                       driver.driverName.data(), driver.driverInfo.data(),
                       driver.conformanceVersion.major, driver.conformanceVersion.minor,
                       driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
}

void LogMemory(const Device& d) {
    const auto chain =
        d.gpu.getMemoryProperties2<vk::PhysicalDeviceMemoryProperties2,
                                   vk::PhysicalDeviceMemoryBudgetPropertiesEXT>(d.dispatch);
    const auto& memory = chain.get<vk::PhysicalDeviceMemoryProperties2>().memoryProperties;
    const auto& budget = chain.get<vk::PhysicalDeviceMemoryBudgetPropertiesEXT>();
    for (u32 heap = 0; heap < memory.memoryHeapCount; heap++) {
        std::string types;
        for (u32 type = 0; type < memory.memoryTypeCount; type++) {
            if (memory.memoryTypes[type].heapIndex == heap) {
                types += fmt::format(" [type {} {}]", type,
                                     vk::to_string(memory.memoryTypes[type].propertyFlags));
            }
        }
        LOG_INFO(Render_Vulkan,
                 "GR2port catcher: memory heap {}: {:.0f} MB {}, budget {:.0f} MB, in use {:.0f} "
                 "MB;{}",
                 heap, Megabytes(memory.memoryHeaps[heap].size),
                 vk::to_string(memory.memoryHeaps[heap].flags),
                 d.memory_budget ? Megabytes(budget.heapBudget[heap]) : 0.0f,
                 d.memory_budget ? Megabytes(budget.heapUsage[heap]) : 0.0f, types);
    }
}

void AddLine(std::string line) {
    const std::scoped_lock lock{g_watch_mutex};
    constexpr size_t KeptLines = 24;
    if (g_watch_lines.size() == KeptLines) {
        g_watch_lines.erase(g_watch_lines.begin());
    }
    g_watch_lines.push_back(std::move(line));
}

void LogWatch(std::string line, bool about_gpu) {
    if (g_watch_count.fetch_add(1) >= MaxWatchLines) {
        return;
    }
    LOG_WARNING(Render_Vulkan, "GR2port catcher: [{:.3f} s] {}",
                std::chrono::duration<double>(Clock::now() - g_began).count(), line);
    Common::Log::Flush();
    if (about_gpu) {
        AddLine(std::move(line));
    }
}

// The GPU's progress with the frame in a slot, in words.
std::string DescribeProgress(u32 slot) {
    const PassTimes::Progress progress = PassTimes::Poll(slot);
    if (!progress.known) {
        return fmt::format("slot {}: the GPU's marks cannot be read", slot);
    }
    if (progress.done) {
        return fmt::format("slot {}: game frame {} is done", slot, progress.game_frame);
    }
    if (progress.finished == 0) {
        return fmt::format("slot {}: game frame {} is not done: the GPU has not begun it", slot,
                           progress.game_frame);
    }
    return fmt::format(
        "slot {}: game frame {} is not done: the GPU finished {} of its {} marks, the last \"{}\", "
        "and is in \"{}\"",
        slot, progress.game_frame, progress.finished, progress.written,
        progress.last < Passes ? PassTimes::Name(static_cast<PassTimes::Pass>(progress.last))
                               : "the frame's start",
        progress.next < Passes ? PassTimes::Name(static_cast<PassTimes::Pass>(progress.next))
                               : "what follows the last mark");
}

std::string DescribeTrace() {
    std::array<u32, TracePasses> crumbs{};
    if (!DeviceLost::Crumbs(crumbs)) {
        return "no pass trace";
    }
    const u32 begun = crumbs[static_cast<u32>(DeviceLost::Pass::FrameStart)];
    u32 last = 0;
    for (u32 pass = 0; pass < TracePasses; pass++) {
        if (crumbs[pass] == begun) {
            last = pass;
        }
    }
    return fmt::format("the pass trace: frame {} began \"{}\" and no later pass", begun,
                       DeviceLost::Name(static_cast<DeviceLost::Pass>(last)));
}

void Beat() {
    g_watch_beat.store(Ticks(Clock::now()), std::memory_order_relaxed);
    Outside::Set(Outside::Crumb::WatchBeat, 0);
    Outside::Set(Outside::Crumb::GameFrames, Capture::Frames());
}

void LogLines(std::string_view title, const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (g_closer_lines.fetch_add(1) >= MaxCloserLines) {
            break;
        }
        LOG_WARNING(Render_Vulkan, "GR2port catcher: {}: {}", title, line);
    }
    Common::Log::Flush();
}

// Asks the GPU for a small job on the second queue and waits two seconds for it: a GPU that still
// runs it is alive and has one job it does not finish; one that does not is doing nothing for
// this process, unless its driver runs a family's queues one after another.
std::string Probe() {
    const Device* const d = g_device;
    if (d == nullptr || !d->probe_queue || g_memory == nullptr) {
        return "the GPU has no second queue to probe it with";
    }
    static vk::CommandPool pool;
    static vk::CommandBuffer cmd;
    static vk::Fence fence;
    static Buffer target;
    static bool pending = false;
    const auto& dl = d->dispatch;
    constexpr u64 TargetBytes = 64;
    if (!pool) {
        pool = d->device
                   .createCommandPool({.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                       .queueFamilyIndex = d->queue_family},
                                      nullptr, dl)
                   .value;
        const auto buffers =
            d->device.allocateCommandBuffers({.commandPool = pool,
                                              .level = vk::CommandBufferLevel::ePrimary,
                                              .commandBufferCount = 1},
                                             dl);
        fence = d->device.createFence({}, nullptr, dl).value;
        target = g_memory->CreateBuffer(TargetBytes, vk::BufferUsageFlagBits::eTransferDst,
                                        Placement::Device);
        if (!pool || buffers.result != vk::Result::eSuccess || !fence || !target.buffer) {
            pool = nullptr;
            return "the probe could not be made";
        }
        cmd = buffers.value.front();
    }
    if (pending) {
        if (d->device.getFenceStatus(fence, dl) != vk::Result::eSuccess) {
            return "the earlier probe's small job is still not done";
        }
        pending = false;
    }
    (void)d->device.resetFences(fence, dl);
    (void)cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    cmd.fillBuffer(target.buffer, 0, TargetBytes, 1, dl);
    (void)cmd.end(dl);
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    const Clock::time_point start = Clock::now();
    const vk::Result submitted = d->probe_queue.submit(submit, fence, dl);
    if (submitted != vk::Result::eSuccess) {
        return fmt::format("the driver refused a small job for the second queue: {}",
                           vk::to_string(submitted));
    }
    constexpr u64 WaitNanoseconds = 2'000'000'000;
    const vk::Result waited = d->device.waitForFences(fence, true, WaitNanoseconds, dl);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (waited == vk::Result::eSuccess) {
        return fmt::format("the GPU ran a small job on a second queue in {:.1f} ms: it and its "
                           "driver still work for this process",
                           ms);
    }
    if (waited == vk::Result::eTimeout) {
        pending = true;
        return "the GPU did not run a small job on a second queue within 2 s: it does nothing "
               "more for this process, or its driver runs these queues one after another";
    }
    return fmt::format("the wait for a small job on a second queue gave {}", vk::to_string(waited));
}

// A closer look at a process that has stopped moving: each GPU's load, what every thread is in,
// and what Windows and the drivers recorded.
void LookCloser(std::string_view why, bool gpu) {
    const u32 look = g_closer_looks.fetch_add(1);
    if (look >= MaxCloserLooks) {
        return;
    }
    LogWatch(fmt::format("closer look {} of at most {}: {}", look + 1, MaxCloserLooks, why), false);
    Beat();
    LogLines("load", {System::LoadNow()});
    LogLines("probe", {Probe()});
    Beat();
    LogLines("history", History());
    LogLines("stack", Stacks::All());
    Beat();
    if (gpu) {
        DeviceLost::ReportStall();
        Beat();
    }
    LogLines("system", System::AtEvent(15));
    Beat();
}

// Watches the watch: a watch that has stopped is a finding too, and a thread that its read of
// the stacks left stopped runs again. It also reads the system's records at the start, which
// takes a while.
void Guard() {
    LogLines("system at the start", System::AtStart());
    LOG_INFO(Render_Vulkan, "GR2port catcher: the executable {} its functions' names",
             Stacks::Named() ? "carries" : "does not carry");
    std::chrono::milliseconds next = GuardFirst;
    // The guard runs on after a lost device, whose report ends the watch: the watcher outside
    // still hears from the process.
    for (;;) {
        std::this_thread::sleep_for(GuardTick);
        Crash::Keep();
        Outside::Set(Outside::Crumb::GuardBeat, 0);
        const auto quiet =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds{
                Ticks(Clock::now()) - g_watch_beat.load(std::memory_order_relaxed)});
        if (quiet < GuardFirst || g_lost.load(std::memory_order_relaxed)) {
            next = GuardFirst;
            continue;
        }
        if (quiet < next) {
            continue;
        }
        next *= 2;
        const bool cut = Stacks::Rescue();
        LOG_WARNING(Render_Vulkan, "GR2port catcher: the watch itself has not run for {} ms{}; {}",
                    quiet.count(), cut ? ", in its read of the stacks, which is now cut short" : "",
                    System::LoadNow());
        Common::Log::Flush();
    }
}

void Watch() {
    u64 seen_submits = 0;
    bool stall_looked = false;
    bool stall_looked_again = false;
    bool blocked_looked = false;
    bool silence_looked = false;
    bool looked = false;
    // The frame the watch last logged as late, until the GPU finishes it.
    bool stalled = false;
    u32 stalled_slot = 0;
    u64 stalled_frame = 0;
    Clock::time_point stalled_since;
    std::chrono::milliseconds next_stall = FirstStall;
    u32 blocked_call = 0;
    std::chrono::milliseconds next_blocked = FirstBlocked;
    u64 game_frames = Capture::Frames();
    Clock::time_point game_moved = Clock::now();
    std::chrono::milliseconds next_silence = FirstSilence;
    while (!g_lost.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(Tick);
        Beat();
        Test::Watch();
        const Clock::time_point now = Clock::now();
        const u64 submits = g_submits.load(std::memory_order_relaxed);
        if (stalled) {
            // A slot that holds another frame has finished the late one.
            const PassTimes::Progress late = PassTimes::Poll(stalled_slot);
            if (late.known && (late.done || late.game_frame != stalled_frame)) {
                LogWatch(
                    fmt::format(
                        "the GPU finished game frame {}, {} ms after the watch first logged "
                        "it as late",
                        stalled_frame,
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - stalled_since)
                            .count()),
                    true);
                stalled = false;
                stall_looked = false;
                stall_looked_again = false;
            }
        }
        if (submits != seen_submits) {
            seen_submits = submits;
            looked = false;
            next_stall = FirstStall;
        }
        const auto since_submit =
            std::chrono::microseconds{Ticks(now) - g_submit_time.load(std::memory_order_relaxed)};
        if (submits != 0 && !looked && since_submit >= FirstLook) {
            const u32 slot = g_submit_slot.load(std::memory_order_relaxed);
            const PassTimes::Progress progress = PassTimes::Poll(slot);
            if (!progress.known) {
                // Without the marks the watch cannot tell a busy GPU from one with nothing to do.
                looked = true;
            } else if (progress.done) {
                // Nothing more to see until the next frame.
                looked = true;
            } else if (since_submit >= next_stall) {
                next_stall *= 2;
                if (!stalled) {
                    // The frame the GPU is in is the older of those it has not finished.
                    stalled = true;
                    Outside::Set(Outside::Crumb::GpuStalled, progress.game_frame);
                    stalled_since = now;
                    stalled_slot = slot;
                    stalled_frame = progress.game_frame;
                    for (u32 other = 0; other < FrameSlots; other++) {
                        const PassTimes::Progress older = PassTimes::Poll(other);
                        if (other != slot && older.known && !older.done &&
                            older.game_frame < stalled_frame) {
                            stalled_slot = other;
                            stalled_frame = older.game_frame;
                        }
                    }
                }
                std::string line = fmt::format("the GPU has not finished a frame for {} ms. {}",
                                               since_submit.count() / 1000, DescribeProgress(slot));
                for (u32 other = 0; other < FrameSlots; other++) {
                    if (other != slot) {
                        line += ". " + DescribeProgress(other);
                    }
                }
                const u32 call = g_call.load(std::memory_order_relaxed);
                line += fmt::format(
                    ". {}. {}. The render thread has been in {} for {:.0f} ms, and the driver has "
                    "answered its wait {} times with \"not yet\"; the game has started {} frames",
                    DescribeTrace(), Marks::DescribeProgress(), CallNames[call],
                    MsSince(g_call_since.load(std::memory_order_relaxed)),
                    g_wait_answers.load(std::memory_order_relaxed), Capture::Frames());
                LogWatch(std::move(line), true);
                if (since_submit >= CloserLook && !stall_looked) {
                    stall_looked = true;
                    LookCloser("the GPU has had a frame for over a second", true);
                } else if (since_submit >= CloserLookAgain && !stall_looked_again) {
                    stall_looked_again = true;
                    LookCloser("the GPU still has that frame", true);
                }
            }
        }
        const u32 call = g_call.load(std::memory_order_relaxed);
        const auto in_call =
            std::chrono::microseconds{Ticks(now) - g_call_since.load(std::memory_order_relaxed)};
        if (call != blocked_call || call == static_cast<u32>(Call::Idle)) {
            blocked_call = call;
            next_blocked = FirstBlocked;
            blocked_looked = false;
        } else if (in_call >= next_blocked) {
            next_blocked *= 2;
            LogWatch(fmt::format("the render thread has been in {} for {} ms", CallNames[call],
                                 in_call.count() / 1000),
                     false);
            if (in_call >= 2 * CloserLook && !blocked_looked && !stall_looked) {
                blocked_looked = true;
                LookCloser("the render thread has been in one call for over two seconds", false);
            }
        }
        const u64 frames = Capture::Frames();
        if (frames != game_frames) {
            game_frames = frames;
            game_moved = now;
            next_silence = FirstSilence;
            silence_looked = false;
        } else if (now - game_moved >= next_silence) {
            next_silence *= 2;
            LogWatch(
                fmt::format(
                    "the game has started no frame for {} ms (frame {})",
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - game_moved).count(),
                    frames),
                false);
            if (frames > BootFrames && !silence_looked) {
                silence_looked = true;
                LookCloser("the game starts no frames", false);
            }
        }
    }
}

std::string DescribeKept(const Kept& kept) {
    std::string line = fmt::format(
        "frame {} drew game frame {}: {}, {} draws, {} casters, {} indices, {} particle draws, {} "
        "mesh samples, {} effect draws; {}; memory {} of {} MB; CPU ms",
        kept.frame, kept.game_frame, kept.world ? "the world" : "the screen alone", kept.draws,
        kept.casters, kept.indices, kept.particle_draws, kept.samples, kept.effect_draws,
        kept.submitted ? "sent" : "not sent", kept.used_mb, kept.budget_mb);
    for (u32 call = 0; call < Calls; call++) {
        line += fmt::format(" {} {:.2f}", CallColumns[call], kept.cpu_ms[call]);
    }
    if (!kept.timed) {
        return line + "; GPU ms not known";
    }
    line += fmt::format("; GPU ms total {:.3f}", kept.gpu_ms[0]);
    return line;
}

} // namespace

void ExtendInstance(std::vector<const char*>& extensions,
                    const vk::detail::DispatchLoaderDynamic& dispatch) {
    const auto [result, available] = vk::enumerateInstanceExtensionProperties(nullptr, dispatch);
    if (result != vk::Result::eSuccess) {
        return;
    }
    const bool found = std::ranges::any_of(available, [](const vk::ExtensionProperties& e) {
        return std::string_view{e.extensionName.data()} == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    });
    const bool listed = std::ranges::any_of(extensions, [](const char* name) {
        return std::string_view{name} == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    });
    if (found && !listed) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
}

void LogInstance(const Device& d) {
    const auto& dl = d.dispatch;
    const auto [version_result, version] = vk::enumerateInstanceVersion(dl);
    LOG_INFO(Render_Vulkan, "GR2port catcher: this build catches lost devices; Vulkan loader {}",
             version_result == vk::Result::eSuccess ? Version(version) : "of no known version");
    if (const auto [result, layers] = vk::enumerateInstanceLayerProperties(dl);
        result == vk::Result::eSuccess) {
        LOG_INFO(Render_Vulkan,
                 "GR2port catcher: {} layers the loader knows; those that are on by themselves "
                 "sit between the game and the driver",
                 layers.size());
        for (const vk::LayerProperties& layer : layers) {
            LOG_INFO(Render_Vulkan, "GR2port catcher: layer {} {} (for Vulkan {}): {}",
                     layer.layerName.data(), layer.implementationVersion,
                     Version(layer.specVersion), layer.description.data());
        }
    }
    if (const auto [result, extensions] = vk::enumerateInstanceExtensionProperties(nullptr, dl);
        result == vk::Result::eSuccess) {
        std::vector<std::string> names;
        for (const vk::ExtensionProperties& extension : extensions) {
            names.emplace_back(extension.extensionName.data());
        }
        LogList("instance extensions", names);
    }
    if (const auto [result, gpus] = d.instance.enumeratePhysicalDevices(dl);
        result == vk::Result::eSuccess) {
        for (size_t i = 0; i < gpus.size(); i++) {
            LOG_INFO(Render_Vulkan, "GR2port catcher: GPU {} of {}: {}", i, gpus.size(),
                     DescribeGpu(d, gpus[i]));
        }
    }
    if (dl.vkCreateDebugUtilsMessengerEXT == nullptr) {
        LOG_INFO(Render_Vulkan, "GR2port catcher: the driver's messages cannot be taken");
        return;
    }
    const VkDebugUtilsMessengerCreateInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = &OnMessage,
    };
    const VkResult made =
        dl.vkCreateDebugUtilsMessengerEXT(d.instance, &info, nullptr, &g_messenger);
    LOG_INFO(Render_Vulkan, "GR2port catcher: the driver's messages {} the log",
             made == VK_SUCCESS ? "come into" : "could not be taken into");
}

void LogDevice(const Device& d) {
    g_device = &d;
    const auto& dl = d.dispatch;
    LOG_INFO(Render_Vulkan, "GR2port catcher: the picked GPU: {}", DescribeGpu(d, d.gpu));
    const auto features =
        d.gpu.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                           vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>(
            dl);
    // Each structure starts with its type and the next structure's address.
    constexpr size_t Head = 16;
    LOG_INFO(Render_Vulkan,
             "GR2port catcher: features, in the Vulkan header's order: 1.0 {}; 1.1 {}; 1.2 {}; "
             "1.3 {}",
             Switches(&features.get<vk::PhysicalDeviceFeatures2>().features,
                      sizeof(vk::PhysicalDeviceFeatures), 0),
             Switches(&features.get<vk::PhysicalDeviceVulkan11Features>(),
                      sizeof(vk::PhysicalDeviceVulkan11Features), Head),
             Switches(&features.get<vk::PhysicalDeviceVulkan12Features>(),
                      sizeof(vk::PhysicalDeviceVulkan12Features), Head),
             Switches(&features.get<vk::PhysicalDeviceVulkan13Features>(),
                      sizeof(vk::PhysicalDeviceVulkan13Features), Head));
    const auto properties =
        d.gpu.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
                             vk::PhysicalDeviceVulkan12Properties,
                             vk::PhysicalDeviceVulkan13Properties>(dl);
    const auto& core = properties.get<vk::PhysicalDeviceProperties2>().properties;
    const auto& core11 = properties.get<vk::PhysicalDeviceVulkan11Properties>();
    const auto& limits = core.limits;
    LOG_INFO(Render_Vulkan,
             "GR2port catcher: limits: {} memory allocations, {} sampler allocations, buffer and "
             "image granularity {}, compute groups {}x{}x{} of {} threads, subgroups of {}, push "
             "constants {} bytes, {} bound sets, timestamps every {} ns, anisotropy {}, image "
             "side {}, viewport {}x{}, non-coherent atom {}, lines {} to {}",
             limits.maxMemoryAllocationCount, limits.maxSamplerAllocationCount,
             limits.bufferImageGranularity, limits.maxComputeWorkGroupCount[0],
             limits.maxComputeWorkGroupCount[1], limits.maxComputeWorkGroupCount[2],
             limits.maxComputeWorkGroupInvocations, core11.subgroupSize,
             limits.maxPushConstantsSize, limits.maxBoundDescriptorSets, limits.timestampPeriod,
             limits.maxSamplerAnisotropy, limits.maxImageDimension2D,
             limits.maxViewportDimensions[0], limits.maxViewportDimensions[1],
             limits.nonCoherentAtomSize, limits.lineWidthRange[0], limits.lineWidthRange[1]);
    LOG_INFO(Render_Vulkan, "GR2port catcher: properties 1.0, the header's structure as bytes: {}",
             Hex(&core, sizeof(core)));
    LOG_INFO(Render_Vulkan, "GR2port catcher: properties 1.1 as bytes: {}",
             Hex(&core11, sizeof(core11)));
    LOG_INFO(Render_Vulkan, "GR2port catcher: properties 1.2 as bytes: {}",
             Hex(&properties.get<vk::PhysicalDeviceVulkan12Properties>(),
                 sizeof(vk::PhysicalDeviceVulkan12Properties)));
    LOG_INFO(Render_Vulkan, "GR2port catcher: properties 1.3 as bytes: {}",
             Hex(&properties.get<vk::PhysicalDeviceVulkan13Properties>(),
                 sizeof(vk::PhysicalDeviceVulkan13Properties)));
    LogMemory(d);
    const auto families = d.gpu.getQueueFamilyProperties(dl);
    for (size_t i = 0; i < families.size(); i++) {
        LOG_INFO(Render_Vulkan,
                 "GR2port catcher: queue family {}{}: {} queues, {}, timestamps of {} bits", i,
                 i == d.queue_family ? " (the renderer's)" : "", families[i].queueCount,
                 vk::to_string(families[i].queueFlags), families[i].timestampValidBits);
    }
    if (const auto [result, extensions] = d.gpu.enumerateDeviceExtensionProperties(nullptr, dl);
        result == vk::Result::eSuccess) {
        std::vector<std::string> names;
        for (const vk::ExtensionProperties& extension : extensions) {
            names.push_back(
                fmt::format("{} {}", extension.extensionName.data(), extension.specVersion));
        }
        LogList("device extensions", names);
    }
    if (const auto [result, surface] = d.gpu.getSurfaceCapabilitiesKHR(d.surface, dl);
        result == vk::Result::eSuccess) {
        LOG_INFO(Render_Vulkan,
                 "GR2port catcher: surface: {} to {} images, now {}x{}, from {}x{} to {}x{}, "
                 "transforms {}, now {}, alpha {}, usage {}",
                 surface.minImageCount, surface.maxImageCount, surface.currentExtent.width,
                 surface.currentExtent.height, surface.minImageExtent.width,
                 surface.minImageExtent.height, surface.maxImageExtent.width,
                 surface.maxImageExtent.height, vk::to_string(surface.supportedTransforms),
                 vk::to_string(surface.currentTransform),
                 vk::to_string(surface.supportedCompositeAlpha),
                 vk::to_string(surface.supportedUsageFlags));
    }
    if (const auto [result, formats] = d.gpu.getSurfaceFormatsKHR(d.surface, dl);
        result == vk::Result::eSuccess) {
        std::vector<std::string> names;
        for (const vk::SurfaceFormatKHR& format : formats) {
            names.push_back(fmt::format("{} {}", vk::to_string(format.format),
                                        vk::to_string(format.colorSpace)));
        }
        LogList("surface formats", names);
    }
    if (const auto [result, modes] = d.gpu.getSurfacePresentModesKHR(d.surface, dl);
        result == vk::Result::eSuccess) {
        std::vector<std::string> names;
        for (const vk::PresentModeKHR mode : modes) {
            names.push_back(vk::to_string(mode));
        }
        LogList("present modes", names);
    }
    LOG_INFO(Render_Vulkan,
             "GR2port catcher: the device has checkpoints {}, fault information {}, the fault's "
             "binary {}, the driver's own diagnostics {}, buffer markers {}, memory budget {}",
             d.checkpoints, d.device_fault, d.fault_binary, d.driver_diagnostics, d.buffer_marker,
             d.memory_budget);
    LogSystem();
    Common::Log::Flush();
}

void Start(Memory& memory) {
    Marks::Start(memory);
    g_memory = &memory;
    g_since = Clock::now();
    g_call_since.store(Ticks(g_since));
    Test::Start(memory);
    Beat();
    g_watch = std::thread{Watch};
    g_watch.detach();
    g_guard = std::thread{Guard};
    g_guard.detach();
    System::StartSampling();
    LOG_INFO(Render_Vulkan,
             "GR2port catcher: the watch looks every {} ms at a frame the GPU has had for {} ms, "
             "and logs one it has had for {} ms",
             Tick.count(), FirstLook.count(), FirstStall.count());
}

void WaitAnswered() {
    g_wait_answers.fetch_add(1, std::memory_order_relaxed);
}

void Enter(Call call) {
    Outside::Set(Outside::Crumb::RenderCall, static_cast<u64>(call));
    if (call == Call::Wait) {
        g_wait_answers.store(0, std::memory_order_relaxed);
    }
    const Clock::time_point now = Clock::now();
    const u32 before = g_call.load(std::memory_order_relaxed);
    g_current.cpu_ms[before] += std::chrono::duration<float, std::milli>(now - g_since).count();
    g_since = now;
    g_call.store(static_cast<u32>(call), std::memory_order_relaxed);
    g_call_since.store(Ticks(now), std::memory_order_relaxed);
}

// Notes what changed from the last frame to this one.
void NoteChanges(const Scene::Header& header) {
    static bool world = false;
    static u32 water = 0;
    static u32 lights = 0;
    static u32 generation = 0;
    if ((header.ui.only == 0) != world) {
        world = !world;
        Note(world ? "the world is drawn" : "only the screen is drawn", header.draws);
    }
    if ((header.effects.water_draws != 0) != (water != 0)) {
        Note(water == 0 ? "water draws begin" : "water draws end", header.effects.water_draws);
    }
    water = header.effects.water_draws;
    if ((header.point_lights != 0) != (lights != 0)) {
        Note(lights == 0 ? "point lights begin" : "point lights end", header.point_lights);
    }
    lights = header.point_lights;
    if (header.particles.generation != generation) {
        generation = header.particles.generation;
        Note("the particle library changes, as an area loads", generation);
    }
    constexpr u32 GapNs = 400'000'000;
    if (header.frame_gap_ns > GapNs) {
        Note("a gap in the game's frames, ms", header.frame_gap_ns / 1'000'000);
    }
}

void Note(std::string_view what, u64 value) {
    Event event{.time = Clock::now(), .game_frame = Capture::Frames(), .value = value};
    what.copy(event.what.data(), EventChars - 1);
    const std::scoped_lock lock{g_events_mutex};
    g_events[g_event_next] = event;
    g_event_next = (g_event_next + 1) % Events;
    g_event_count = std::min(g_event_count + 1, Events);
}

void NotePad(int pad, u32 buttons) {
    static std::atomic<u32> noted{0};
    if (pad == 0 && noted.exchange(buttons, std::memory_order_relaxed) != buttons) {
        Note("the pad's buttons change to", buttons);
    }
}

std::vector<std::string> History() {
    std::vector<std::string> lines;
    const Clock::time_point now = Clock::now();
    const std::scoped_lock lock{g_events_mutex};
    lines.reserve(g_event_count);
    for (size_t back = g_event_count; back > 0; back--) {
        const Event& event = g_events[(g_event_next + Events - back) % Events];
        lines.push_back(fmt::format("{:.3f} s before, game frame {}: {} ({})",
                                    std::chrono::duration<double>(now - event.time).count(),
                                    event.game_frame, event.what.data(), event.value));
    }
    return lines;
}

void BeginFrame(const Scene::Header& header) {
    Crash::Keep();
    NoteChanges(header);
    Test::Render(header.frame);
    // The time in Idle since the last frame stays in this frame's line.
    const float idle = g_current.cpu_ms[static_cast<u32>(Call::Idle)];
    g_current = {};
    g_current.cpu_ms[static_cast<u32>(Call::Idle)] = idle;
    g_current.game_frame = header.frame;
    g_current.indices = header.indices;
    g_current.draws = header.draws;
    g_current.casters = header.casters;
    g_current.particle_draws = header.particles.draws;
    g_current.samples = header.particles.samples;
    g_current.effect_draws = header.effects.draws;
    g_current.world = header.ui.only == 0;
}

void EndFrame(bool submitted, u32 slot) {
    Enter(Call::Idle);
    g_current.frame = DeviceLost::Frame();
    g_current.submitted = submitted;
    if (g_memory != nullptr) {
        const Memory::Budget budget = g_memory->DeviceBudget();
        g_current.used_mb = static_cast<u32>(budget.usage >> 20);
        g_current.budget_mb = static_cast<u32>(budget.budget >> 20);
    }
    g_kept[g_kept_count++ % KeptFrames] = g_current;
    g_current = {};
    if (submitted) {
        g_submit_slot.store(slot, std::memory_order_relaxed);
        g_submit_time.store(Ticks(Clock::now()), std::memory_order_relaxed);
        g_submits.fetch_add(1, std::memory_order_relaxed);
    }
}

void FrameTimes(u64 game_frame, std::span<const double> ms) {
    constexpr u64 Looked = 8;
    for (u64 back = 1; back <= std::min<u64>(Looked, g_kept_count); back++) {
        Kept& kept = g_kept[(g_kept_count - back) % KeptFrames];
        if (kept.game_frame != game_frame || !kept.submitted || kept.timed) {
            continue;
        }
        kept.timed = true;
        for (size_t i = 0; i < std::min(ms.size(), kept.gpu_ms.size()); i++) {
            kept.gpu_ms[i] = static_cast<float>(ms[i]);
        }
        return;
    }
}

void ReportLoss(const Device& device) {
    g_lost.store(true, std::memory_order_relaxed);
    LogLines("history at the loss", History());
    LogLines("stack at the loss", Stacks::All());
    LogLines("system at the loss", System::AtEvent(15));
    LOG_ERROR(Render_Vulkan, "GR2port catcher: the driver sent {} messages in this run",
              g_messages.load());
    {
        const std::scoped_lock lock{g_watch_mutex};
        LOG_ERROR(Render_Vulkan,
                  "GR2port catcher: the watch's last {} lines about frames the GPU did not finish",
                  g_watch_lines.size());
        for (const std::string& line : g_watch_lines) {
            LOG_ERROR(Render_Vulkan, "GR2port catcher: earlier: {}", line);
        }
    }
    for (u32 slot = 0; slot < FrameSlots; slot++) {
        LOG_ERROR(Render_Vulkan, "GR2port catcher: now: {}", DescribeProgress(slot));
    }
    LOG_ERROR(Render_Vulkan, "GR2port catcher: now: {}", DescribeTrace());
    Marks::Report();
    Common::Log::Flush();
    const u64 count = std::min<u64>(g_kept_count, KeptFrames);
    LOG_ERROR(Render_Vulkan, "GR2port catcher: the last {} frames, the oldest first", count);
    for (u64 back = count; back >= 1; back--) {
        const Kept& kept = g_kept[(g_kept_count - back) % KeptFrames];
        std::string line = DescribeKept(kept);
        if (kept.timed) {
            if (back <= FullFrames) {
                for (u32 pass = 0; pass < Passes; pass++) {
                    line += fmt::format(", {} {:.3f}",
                                        PassTimes::Name(static_cast<PassTimes::Pass>(pass)),
                                        kept.gpu_ms[pass + 1]);
                }
            } else {
                std::array<u32, Passes> order;
                for (u32 pass = 0; pass < Passes; pass++) {
                    order[pass] = pass;
                }
                std::ranges::partial_sort(order, order.begin() + NamedPasses, [&](u32 a, u32 b) {
                    return kept.gpu_ms[a + 1] > kept.gpu_ms[b + 1];
                });
                for (u32 i = 0; i < NamedPasses; i++) {
                    line += fmt::format(", {} {:.3f}",
                                        PassTimes::Name(static_cast<PassTimes::Pass>(order[i])),
                                        kept.gpu_ms[order[i] + 1]);
                }
            }
        }
        LOG_ERROR(Render_Vulkan, "GR2port catcher: {}", line);
    }
    Common::Log::Flush();
    std::vector<std::string> added;
    for (std::string& module : Modules()) {
        if (std::ranges::find(g_modules, module) == g_modules.end()) {
            added.push_back(std::move(module));
        }
    }
    LogList("files loaded into the process since the start", added);
    LogMemory(device);
#ifdef __linux__
    LOG_ERROR(Render_Vulkan,
              "GR2port catcher: the kernel's own report is not in this log. Right after a freeze, "
              "before a restart of the PC, run in a terminal: journalctl -k -b --no-pager | grep "
              "-iE \"i915|xe|drm|amdgpu|nvidia|gpu|hang|reset\" | tail -n 200 > kernel.txt, and "
              "send kernel.txt with this log");
#endif
    Common::Log::Flush();
}

} // namespace Gr2Port::Catcher
