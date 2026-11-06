// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "video_core/gr2port/overlays.h"

namespace Gr2Port {

void KeepOverlaysOut() {
#ifdef _WIN32
    // RTSS enters through a global window hook and an implicit Vulkan layer. With a second device
    // in the process, the driver faults inside RTSS's hook of the present.
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY policy{};
    policy.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &policy, sizeof(policy));
    SetEnvironmentVariableW(L"DISABLE_RTSS_LAYER", L"1");
#endif
}

void LogOverlays() {
#ifdef _WIN32
    LOG_INFO(Render_Vulkan, "GR2port: RTSS hooks in the process: {}",
             GetModuleHandleW(L"RTSSHooks64.dll") != nullptr);
#endif
}

} // namespace Gr2Port
