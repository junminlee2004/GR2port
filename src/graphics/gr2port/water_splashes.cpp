// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/water_splashes.h"

namespace Gr2Port::WaterSplashes {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// FUN_01527a30's loop over the feet, for a fast foot, with the foot's number in r13 and the
// splash's maker in rdi: test rdi, rdi; je 0x152a5a0 goes on to the next foot without a splash
// when there is no maker. Both ways on write rax and xmm0 before they read them.
constexpr Hooks::Entry Feet{0x1529f18, 9, {0x48, 0x85, 0xff, 0x0f, 0x84, 0x7f, 0x06, 0x00, 0x00}};
constexpr VAddr FeetSplash = 0x1529f21;
constexpr VAddr FeetNext = 0x152a5a0;

// FUN_01527a30's path for a single point, the same for a fast point: test rdi, rdi;
// je 0x152acc0.
constexpr Hooks::Entry Point{0x152a695, 9, {0x48, 0x85, 0xff, 0x0f, 0x84, 0x22, 0x06, 0x00, 0x00}};
constexpr VAddr PointSplash = 0x152a69e;
constexpr VAddr PointNext = 0x152acc0;

// The frames per second the splashes are made for.
constexpr float SplashRate = 30.0f;

// The share of a splash that each fast point holds: the two feet, then the single point.
std::array<float, 3> g_shares{};
constexpr u32 PointShare = 2;

// The two stubs and their constants.
constexpr size_t StubBytes = 192;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) {
        return reinterpret_cast<const u8*>(module_base + address - GoldHenBase);
    };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the water splashes' hooks");
        return false;
    }
    Xbyak::Label splash_rate;
    Xbyak::Label one;
    Xbyak::Label half;
    // Each fast frame adds the step's time in 30 fps frames to the point's share, which holds at
    // most one splash. The point makes a splash when it holds half of one or more, and the splash
    // takes one away. At 30 fps each frame adds a whole splash.
    const auto stub = [&](const Xbyak::Address& share, VAddr splash, VAddr next) {
        const void* start = c.getCurr();
        Xbyak::Label hold;
        c.test(rdi, rdi);
        c.jz(code(next));
        c.mov(rax, reinterpret_cast<u64>(g_shares.data()));
        c.vmovss(xmm0, dword[rip + code(StepTime)]);
        c.vmulss(xmm0, xmm0, dword[rip + splash_rate]);
        c.vaddss(xmm0, xmm0, share);
        c.vminss(xmm0, xmm0, dword[rip + one]);
        c.vucomiss(xmm0, dword[rip + half]);
        c.jb(hold, Xbyak::CodeGenerator::T_NEAR);
        c.vsubss(xmm0, xmm0, dword[rip + one]);
        c.vmovss(share, xmm0);
        c.jmp(code(splash));
        c.L(hold);
        c.vmovss(share, xmm0);
        c.jmp(code(next));
        return start;
    };
    const void* feet = stub(dword[rax + r13 * 4], FeetSplash, FeetNext);
    const void* point = stub(dword[rax + PointShare * 4], PointSplash, PointNext);
    c.L(splash_rate);
    c.dd(std::bit_cast<u32>(SplashRate));
    c.L(one);
    c.dd(std::bit_cast<u32>(1.0f));
    c.L(half);
    c.dd(std::bit_cast<u32>(0.5f));
    if (Hooks::Replace(module_base, Feet, c, capacity, feet) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, Point, c, capacity, point) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: Kat's water splashes are not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::WaterSplashes
