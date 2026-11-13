// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/gravity_slide.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::GravitySlide {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// The frames per second the steering's steps are made for.
constexpr float StepRate = 30.0f;

// FUN_008b3860, the slide's update, with the steering in xmm1, the stick's target in xmm2 and
// the style's fraction in xmm0: vmovss xmm3, [1.0] starts steering = steering * (1 - fraction) +
// target * fraction, which ends with the vaddss at FollowSum. The game writes xmm0 and xmm3
// before it reads them again.
constexpr Hooks::Entry Follow{0x8b6075, 8, {0xc5, 0xfa, 0x10, 0x1d, 0xcb, 0x18, 0x00, 0x01}};
constexpr VAddr FollowSum = 0x8b6089;

// FUN_008c2900, the slide's turn, with the steering in xmm6 and the amount it eases back by in
// xmm1: vmovss [r15 + 0x324], xmm2 stores their sum. The game reads the amount in xmm1 later.
constexpr Hooks::Entry Ease{0x8c2f63, 9, {0xc4, 0xc1, 0x7a, 0x11, 0x97, 0x24, 0x03, 0x00, 0x00}};
constexpr VAddr EaseNext = 0x8c2f6c;
constexpr u32 Steering = 0x324;

// The stack bytes the follow's stub moves down by to keep ymm4: the red zone and the register.
constexpr u32 KeptBytes = 0xa0;

// The stubs and their constants.
constexpr size_t StubBytes = 320;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) {
        return reinterpret_cast<const u8*>(module_base + address - GoldHenBase);
    };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the gravity slide's hooks");
        return false;
    }
    Xbyak::Label step_rate, one, two, half, third;

    // With f the step's time in 30 fps frames, a fraction a of a frame becomes 1 - (1 - a)^f, by
    // its series f * a * (1 + (1 - f) * a / 2 * (1 + (2 - f) * a / 3)): at 30 fps it is a itself.
    const void* follow = c.getCurr();
    c.lea(rsp, ptr[rsp - KeptBytes]);
    c.vmovups(ptr[rsp], ymm4);
    c.vmovss(xmm3, dword[rip + code(StepTime)]);
    c.vmulss(xmm3, xmm3, dword[rip + step_rate]);
    c.vminss(xmm3, xmm3, dword[rip + one]);
    c.vmovss(xmm4, dword[rip + two]);
    c.vsubss(xmm4, xmm4, xmm3);
    c.vmulss(xmm4, xmm4, xmm0);
    c.vmulss(xmm4, xmm4, dword[rip + third]);
    c.vaddss(xmm4, xmm4, dword[rip + one]);
    c.vmulss(xmm4, xmm4, xmm0);
    c.vmulss(xmm4, xmm4, dword[rip + half]);
    c.vmulss(xmm0, xmm0, xmm3);
    c.vsubss(xmm3, xmm3, dword[rip + one]);
    c.vmulss(xmm4, xmm4, xmm3);
    c.vmovss(xmm3, dword[rip + one]);
    c.vsubss(xmm3, xmm3, xmm4);
    c.vmulss(xmm0, xmm0, xmm3);
    // The game's own step, with the new fraction in xmm0.
    c.vmovss(xmm3, dword[rip + one]);
    c.vsubss(xmm3, xmm3, xmm0);
    c.vmulss(xmm0, xmm2, xmm0);
    c.vmulss(xmm1, xmm1, xmm3);
    c.vmovups(ymm4, ptr[rsp]);
    c.lea(rsp, ptr[rsp + KeptBytes]);
    c.jmp(code(FollowSum));

    // The amount times f, then the game's own sum and store.
    const void* ease = c.getCurr();
    c.vmovss(xmm2, dword[rip + code(StepTime)]);
    c.vmulss(xmm2, xmm2, dword[rip + step_rate]);
    c.vminss(xmm2, xmm2, dword[rip + one]);
    c.vmulss(xmm1, xmm1, xmm2);
    c.vaddss(xmm2, xmm6, xmm1);
    c.vmovss(dword[r15 + Steering], xmm2);
    c.jmp(code(EaseNext));

    c.L(step_rate);
    c.dd(std::bit_cast<u32>(StepRate));
    c.L(one);
    c.dd(std::bit_cast<u32>(1.0f));
    c.L(two);
    c.dd(std::bit_cast<u32>(2.0f));
    c.L(half);
    c.dd(std::bit_cast<u32>(0.5f));
    c.L(third);
    c.dd(std::bit_cast<u32>(1.0f / 3.0f));

    if (Hooks::Replace(module_base, Follow, c, capacity, follow) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, Ease, c, capacity, ease) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the gravity slide's steering is not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::GravitySlide
