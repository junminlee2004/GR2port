// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstddef>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common/logging/log.h"
#include "graphics/gr2port/overlays.h"

namespace Gr2Port {

namespace {

// The variables that keep RTSS's and MangoHud's implicit Vulkan layers out of an instance, as
// their layer manifests name them.
constexpr std::array<const char*, 2> LayerSwitches{"DISABLE_RTSS_LAYER", "DISABLE_MANGOHUD"};

std::atomic<bool> g_logged_hooks{false};

// The Vulkan loader reads the process environment itself, so on Windows these go through the
// Win32 block rather than the C runtime's copy.
std::optional<std::string> ReadVariable(const char* name) {
#ifdef _WIN32
    char value[256];
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
        return std::nullopt;
    }
    return std::string(value, length);
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
#endif
}

void WriteVariable(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value ? value->c_str() : nullptr);
#else
    if (value) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

} // namespace

OverlaysOutOfInstance::OverlaysOutOfInstance() {
    for (std::size_t i = 0; i < LayerSwitches.size(); i++) {
        saved[i] = ReadVariable(LayerSwitches[i]);
        WriteVariable(LayerSwitches[i], "1");
    }
}

OverlaysOutOfInstance::~OverlaysOutOfInstance() {
    for (std::size_t i = 0; i < LayerSwitches.size(); i++) {
        WriteVariable(LayerSwitches[i], saved[i]);
    }
}

bool OverlayHooksEveryDevice() {
#ifdef _WIN32
    // RTSS's hooks come in through its global window hook and catch every device's present,
    // whichever instance its layer is in; drawing on the emulator's device faults the driver.
    const bool hooked = GetModuleHandleW(L"RTSSHooks64.dll") != nullptr;
    if (hooked && !g_logged_hooks.exchange(true)) {
        LOG_INFO(Render_Vulkan, "GR2port: RTSS is in the process, so the emulator's window stops "
                                "presenting and RTSS draws on GR2port's");
    }
    return hooked;
#else
    return false;
#endif
}

void LogOverlays() {
#ifdef _WIN32
    LOG_INFO(Render_Vulkan, "GR2port: RTSS hooks in the process: {}",
             GetModuleHandleW(L"RTSSHooks64.dll") != nullptr);
#else
    void* const mangohud = dlopen("libMangoHud.so", RTLD_NOW | RTLD_NOLOAD);
    LOG_INFO(Render_Vulkan, "GR2port: MangoHud in the process: {}", mangohud != nullptr);
    if (mangohud != nullptr) {
        dlclose(mangohud);
    }
#endif
}

} // namespace Gr2Port
