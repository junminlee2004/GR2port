// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/gravity_kick.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::GravityKick {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// FUN_00896fe0, the gravity kick's update: vmovss xmm3, [0x1efb104] loads the step that scales
// the sweep's length and its path.
constexpr Hooks::Entry SweepStep{0x899dbb, 8, {0xc5, 0xfa, 0x10, 0x1d, 0x41, 0x13, 0x66, 0x01}};
constexpr VAddr SweepStepNext = 0x899dc3;

// The step of the frame rate the sweep is made for.
constexpr float SweepStepFloor = 1.0f / 30.0f;

// The stub and its constant.
constexpr size_t StubBytes = 40;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) {
        return reinterpret_cast<const u8*>(module_base + address - GoldHenBase);
    };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the gravity kick's hook");
        return false;
    }
    Xbyak::Label step_floor;
    const void* step = c.getCurr();
    c.vmovss(xmm3, dword[rip + code(StepTime)]);
    c.vmaxss(xmm3, xmm3, dword[rip + step_floor]);
    c.jmp(code(SweepStepNext));
    c.L(step_floor);
    c.dd(std::bit_cast<u32>(SweepStepFloor));
    if (Hooks::Replace(module_base, SweepStep, c, capacity, step) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the gravity kick's sweep is not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::GravityKick
