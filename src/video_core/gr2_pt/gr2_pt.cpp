// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include <xbyak/xbyak.h>

#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "core/libraries/error_codes.h"
#include "core/memory.h"
#include "video_core/gr2_pt/gr2_pt.h"
#include "video_core/gr2_pt/hooks.h"
#include "video_core/gr2_pt/renderer.h"

namespace Gr2Pt {

namespace {

constexpr u64 StubAreaSize = 16_KB;
// Past the module's 8 MB trampoline area and within reach of a rel32 jump from the executable.
constexpr u64 StubAreaOffset = 32_MB;

// The frame graph FUN_0142ec00: the game's draw thread calls it once per frame.
constexpr Hooks::Entry FrameGraph{0x142ec00, 6, Hooks::FramePrologue};

void PS4_SYSV_ABI OnFrameGraph(u32, const Hooks::Registers*, u64) {
    Renderer::OnGameFrame();
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
            Core::MemoryMapFlags::NoFlags, Core::VMAType::Code, "GR2 PT") != ORBIS_OK ||
        reinterpret_cast<VAddr>(area) + StubAreaSize - module_base >
            std::numeric_limits<s32>::max()) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no memory for the hook stubs near the executable");
        return;
    }
    Xbyak::CodeGenerator c(StubAreaSize, area);
    const Hooks::Result frame =
        Hooks::ObserveEntry(module_base, FrameGraph, c, StubAreaSize, &OnFrameGraph, 0);
    if (frame != Hooks::Result::Hooked && frame != Hooks::Result::Chained) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: the frame graph's entry is not the expected code");
        return;
    }
    Renderer::Start();
    LOG_INFO(Render_Vulkan, "GR2 PT: renderer started");
}

} // namespace Gr2Pt
