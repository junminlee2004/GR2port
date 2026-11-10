// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>

#include "common/logging/log.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/gr2port/emulated_gpu.h"
#include "video_core/gr2port/hooks.h"

namespace Gr2Port::EmulatedGpu {

namespace {

std::atomic<bool> g_on{false};
std::atomic<VAddr> g_module_base{0};

// SubmitCategory FUN_014214e0, the frame graph's per-item draw loop. Its handlers read the items
// and write GNM commands, which only the emulation reads.
constexpr Hooks::Entry DrawLoop{0x14214e0, 6, Hooks::FramePrologue};

void UpdateDrawLoop() {
    const VAddr module_base = g_module_base.load(std::memory_order_relaxed);
    if (module_base == 0) {
        return;
    }
    const bool returning = !g_on.load(std::memory_order_relaxed);
    if (Hooks::SetReturning(module_base, DrawLoop, returning) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the draw loop is not the expected code");
    }
}

} // namespace

void Install(VAddr module_base) {
    g_module_base.store(module_base, std::memory_order_relaxed);
    UpdateDrawLoop();
}

bool Wanted() {
    static const bool wanted = [] {
        if (std::getenv("GR2PORT_FORK") != nullptr) {
            return true;
        }
        LOG_INFO(Render_Vulkan, "GR2port: GR2fork's renderer is not created");
        return false;
    }();
    return wanted;
}

void Start(AmdGpu::Liverpool* liverpool) {
    g_on.store(liverpool->GetRasterizer() != nullptr, std::memory_order_relaxed);
    UpdateDrawLoop();
    LOG_INFO(Render_Vulkan, "GR2port: GR2fork's GPU emulation {}", On() ? "on" : "off");
}

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

} // namespace Gr2Port::EmulatedGpu
