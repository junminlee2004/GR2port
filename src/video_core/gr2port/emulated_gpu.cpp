// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>

#include "common/logging/log.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/gr2port/emulated_gpu.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Gr2Port::EmulatedGpu {

namespace {

AmdGpu::Liverpool* g_liverpool = nullptr;
Vulkan::Rasterizer* g_rasterizer = nullptr;
std::atomic<bool> g_on{true};

} // namespace

void Start(AmdGpu::Liverpool* liverpool, Vulkan::Rasterizer* rasterizer) {
    if (liverpool->GetRasterizer() == nullptr) {
        g_on.store(false, std::memory_order_relaxed);
        return;
    }
    g_liverpool = liverpool;
    g_rasterizer = rasterizer;
    if (std::getenv("GR2PORT_NO_FORK") != nullptr) {
        liverpool->BindRasterizer(nullptr);
        g_on.store(false, std::memory_order_relaxed);
        LOG_INFO(Render_Vulkan, "GR2port: GR2fork's GPU emulation starts off");
    }
}

void Toggle() {
    if (g_liverpool == nullptr) {
        return;
    }
    g_liverpool->SendCommand([] {
        const bool on = g_liverpool->GetRasterizer() != nullptr;
        if (on) {
            // What the rasterizer has recorded runs before it stops being called.
            g_rasterizer->Finish();
        }
        g_liverpool->BindRasterizer(on ? nullptr : g_rasterizer);
        g_on.store(!on, std::memory_order_relaxed);
        LOG_INFO(Render_Vulkan, "GR2port: GR2fork's GPU emulation {}", on ? "off" : "on");
    });
}

bool On() {
    return g_on.load(std::memory_order_relaxed);
}

} // namespace Gr2Port::EmulatedGpu
