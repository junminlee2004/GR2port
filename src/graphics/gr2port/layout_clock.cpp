// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/layout_clock.h"

namespace Gr2Port::LayoutClock {

namespace {

// FUN_013e4a90, which steps a layout animation, with the step in ticks in xmm1:
// vaddss xmm1, xmm1, [0.5] rounds it before vcvttss2si eax, xmm1 takes the whole ticks.
constexpr Hooks::Entry StepRounding{0x13e5700, 8, {0xc5, 0xf2, 0x58, 0x0d, 0x54, 0xf6, 0x4d, 0x00}};
constexpr VAddr StepRoundingNext = 0x13e5708;

// The fps patch's running share of a 30 fps frame, from 0 up to 1, a float. It moves on by the
// real frame's time once per game frame.
constexpr VAddr FramePhase = 0x1f7c604;

// FUN_01773340 starts a comic page's caption with a speed in seconds a 30 fps frame: 1/30, and
// -1/6 to play it back when the page goes back before the caption's panel. The fps patch loads
// the frame's time in place of 1/30, and calls its own code for -5 times the frame's time in place
// of -1/6. An animation's step already takes the speed times the frame's share of a 30 fps frame.
constexpr Hooks::Entry CaptionSpeed{0x1773471, 8, {0xc5, 0xfa, 0x10, 0x05, 0x9f, 0x91, 0x80, 0x00}};
constexpr VAddr CaptionSpeedNext = 0x1773479;
constexpr Hooks::Entry CaptionBackSpeed{
    0x17734b6, 8, {0xe8, 0x45, 0xca, 0x2e, 0x00, 0x0f, 0x1f, 0x00}};
constexpr VAddr CaptionBackSpeedNext = 0x17734be;
// The game's two speeds, floats.
constexpr VAddr ForwardSpeed = 0x18c37b4;
constexpr VAddr BackSpeed = 0x18c37b8;

// The stubs and their constants.
constexpr size_t StubBytes = 96;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the layout clock's hook");
        return false;
    }
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };
    Xbyak::Label whole_tick, one, half;

    // A step under one tick keeps the game's rounding.
    const void* stub = c.getCurr();
    c.vcomiss(xmm1, dword[rip + one]);
    c.jae(whole_tick);
    c.vaddss(xmm1, xmm1, dword[rip + half]);
    c.jmp(code(StepRoundingNext));
    c.L(whole_tick);
    c.vaddss(xmm1, xmm1, dword[rip + code(FramePhase)]);
    c.jmp(code(StepRoundingNext));
    c.L(one);
    c.dd(std::bit_cast<u32>(1.0f));
    c.L(half);
    c.dd(std::bit_cast<u32>(0.5f));
    if (Hooks::Replace(module_base, StepRounding, c, capacity, stub) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the layout clock's rounding is not the expected code");
        return false;
    }

    // The caption takes the game's speeds, so its step is the frame's time once.
    const void* forward = c.getCurr();
    c.vmovss(xmm0, dword[rip + code(ForwardSpeed)]);
    c.jmp(code(CaptionSpeedNext));
    const void* back = c.getCurr();
    c.vmovss(xmm0, dword[rip + code(BackSpeed)]);
    c.jmp(code(CaptionBackSpeedNext));
    if (Hooks::Replace(module_base, CaptionSpeed, c, capacity, forward) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, CaptionBackSpeed, c, capacity, back) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the comic caption's speeds are not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::LayoutClock
