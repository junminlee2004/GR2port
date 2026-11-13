// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hair_flutter.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::HairFlutter {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// FUN_01501b20, with the air's speed over the chain's limit in xmm0: vmulss xmm0, xmm0,
// [0x18c5808] starts the phase's advance, that speed times 0.8333334 times the chain's rate.
constexpr Hooks::Entry Advance{0x15023b7, 8, {0xc5, 0xfa, 0x59, 0x05, 0x49, 0x34, 0x3c, 0x00}};
constexpr VAddr AdvanceNext = 0x15023bf;
constexpr VAddr AdvanceScale = 0x18c5808;

// The frames per second the advance is made for.
constexpr float AdvanceRate = 30.0f;

// The stub and its constant.
constexpr size_t StubBytes = 48;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) {
        return reinterpret_cast<const u8*>(module_base + address - GoldHenBase);
    };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the hair's hook");
        return false;
    }
    Xbyak::Label advance_rate;
    const void* advance = c.getCurr();
    c.vmulss(xmm0, xmm0, dword[rip + code(AdvanceScale)]);
    c.vmulss(xmm0, xmm0, dword[rip + code(StepTime)]);
    c.vmulss(xmm0, xmm0, dword[rip + advance_rate]);
    c.jmp(code(AdvanceNext));
    c.L(advance_rate);
    c.dd(std::bit_cast<u32>(AdvanceRate));
    if (Hooks::Replace(module_base, Advance, c, capacity, advance) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the hair's step is not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::HairFlutter
