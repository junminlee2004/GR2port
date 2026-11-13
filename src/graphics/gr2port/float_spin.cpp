// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/float_spin.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::FloatSpin {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// FUN_008e1b80: vmulss xmm2, xmm10, [rbp - 0xa0] makes the angle of the lean in xmm10, times the
// scale of the turns, 1 in the float and 0.55 otherwise. The game writes xmm3 before it reads it
// again.
constexpr Hooks::Entry Turn{0x8e731d, 8, {0xc5, 0xaa, 0x59, 0x95, 0x60, 0xff, 0xff, 0xff}};
constexpr VAddr TurnNext = 0x8e7325;

// The frames per second the turn is made for.
constexpr float FrameRate = 30.0f;

// The stub and its constant.
constexpr size_t StubBytes = 48;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) {
        return reinterpret_cast<const u8*>(module_base + address - GoldHenBase);
    };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the hook of Kat's turn in the air");
        return false;
    }
    Xbyak::Label frame_rate;
    const void* turn = c.getCurr();
    c.vmulss(xmm2, xmm10, dword[rbp - 0xa0]);
    c.vmovss(xmm3, dword[rip + code(StepTime)]);
    c.vmulss(xmm3, xmm3, dword[rip + frame_rate]);
    c.vmulss(xmm2, xmm2, xmm3);
    c.jmp(code(TurnNext));
    c.L(frame_rate);
    c.dd(std::bit_cast<u32>(FrameRate));
    if (Hooks::Replace(module_base, Turn, c, capacity, turn) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: Kat's turn in the air is not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::FloatSpin
