// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include <xbyak/xbyak.h>

#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "core/libraries/error_codes.h"
#include "core/memory.h"
#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/emulated_gpu.h"
#include "graphics/gr2port/gr2port.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/meshes.h"
#include "graphics/gr2port/mouse_camera.h"
#include "graphics/gr2port/movies.h"
#include "graphics/gr2port/player_control.h"
#include "graphics/gr2port/renderer.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/timeline.h"
#include "graphics/gr2port/ui_capture.h"

namespace Gr2Port {

namespace {

constexpr u64 StubAreaSize = 16_KB;
// Past the module's 8 MB trampoline area and within reach of a rel32 jump from the executable.
constexpr u64 StubAreaOffset = 32_MB;

// The frame graph FUN_0142ec00: the game's draw thread calls it once per frame.
constexpr Hooks::Entry FrameGraph{0x142ec00, 6, Hooks::FramePrologue};

// The draw thread's change to its next context, at the end of each frame.
constexpr Hooks::Entry NextContext{0x16147e0, 6, Hooks::FramePrologue};

// The particle manager's first and last steps of a frame, inside the frame graph.
constexpr Hooks::Entry ParticleSetup{0x14a31c0, 6, Hooks::FramePrologue};
constexpr Hooks::Entry ParticleList{0x14a3ba0, 6, Hooks::FramePrologue};

void PS4_SYSV_ABI OnParticleSetup(u32, const Hooks::Registers* registers, u64) {
    Capture::OnParticleSetup(registers->rdi);
}

void PS4_SYSV_ABI OnParticleList(u32, const Hooks::Arguments* arguments, u64) {
    Capture::OnParticleList(arguments->rdi);
}

// FUN_0142ec00(T + 0xa0, view, ...): rdi is the frame graph, rsi the view the frame draws.
void PS4_SYSV_ABI OnFrameGraph(u32, const Hooks::Registers* registers, u64) {
    Capture::OnFrame(registers->rdi, registers->rsi);
    EmulatedGpu::SetFrameGraphRunning(true);
}

void PS4_SYSV_ABI OnFrameGraphEnd(u32, const Hooks::Arguments*, u64) {
    EmulatedGpu::SetFrameGraphRunning(false);
    UiCapture::OnFrameGraphEnd();
}

void PS4_SYSV_ABI OnNextContext(u32, const Hooks::Registers*, u64) {
    Capture::OnFrameEnd();
}

} // namespace

void Install(VAddr module_base) {
    if (module_base != MemoryPatcher::g_eboot_address || !Common::IsGravityRush2()) {
        return;
    }
    void* area =
        reinterpret_cast<void*>(module_base + MemoryPatcher::g_eboot_image_size + StubAreaOffset);
    if (Core::Memory::Instance()->MapMemory(
            &area, reinterpret_cast<VAddr>(area), StubAreaSize,
            Core::MemoryProt::CpuReadWrite | Core::MemoryProt::CpuExec,
            Core::MemoryMapFlags::NoFlags, Core::VMAType::Code, "GR2port") != ORBIS_OK ||
        reinterpret_cast<VAddr>(area) + StubAreaSize - module_base >
            std::numeric_limits<s32>::max()) {
        LOG_ERROR(Render_Vulkan, "GR2port: no memory for the hook stubs near the executable");
        return;
    }
    Xbyak::CodeGenerator c(StubAreaSize, area);
    const Hooks::Result frame =
        Hooks::ObserveEntry(module_base, FrameGraph, c, StubAreaSize, &OnFrameGraph, 0);
    if (frame != Hooks::Result::Hooked && frame != Hooks::Result::Chained) {
        LOG_ERROR(Render_Vulkan, "GR2port: the frame graph's entry is not the expected code");
        return;
    }
    UiCapture::Install(module_base);
    MouseCamera::Install(module_base, c, StubAreaSize);
    Timeline::Install(module_base, c, StubAreaSize);
    PlayerControl::Install(module_base, c, StubAreaSize);
    if (Hooks::ObserveReturn(module_base, FrameGraph, c, StubAreaSize, &OnFrameGraphEnd, 0) ==
            Hooks::Result::Mismatch ||
        Hooks::ObserveEntry(module_base, NextContext, c, StubAreaSize, &OnNextContext, 0) ==
            Hooks::Result::Mismatch) {
        LOG_ERROR(Render_Vulkan, "GR2port: the screen drawing's hooks are not the expected code");
    }
    if (Hooks::ObserveEntry(module_base, ParticleSetup, c, StubAreaSize, &OnParticleSetup, 0) ==
            Hooks::Result::Mismatch ||
        Hooks::ObserveReturn(module_base, ParticleList, c, StubAreaSize, &OnParticleList, 0) ==
            Hooks::Result::Mismatch) {
        LOG_ERROR(Render_Vulkan, "GR2port: the particle manager is not the expected code");
    }
    if (!Meshes::Install(module_base, c, StubAreaSize) ||
        !Textures::Install(module_base, c, StubAreaSize)) {
        return;
    }
    if (!Movies::Install(module_base, c, StubAreaSize)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the movie update is not the expected code");
    }
    EmulatedGpu::Install(module_base, c, StubAreaSize);
    Renderer::Start();
    LOG_INFO(Render_Vulkan, "GR2port: renderer started");
}

} // namespace Gr2Port
