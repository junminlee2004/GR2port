// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/camera_reach.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::CameraReach {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// vaddss xmm0, xmm0, [0x18c5ea8]: the camera's distance in xmm0 plus 0.1 m, which the hit's
// distance in xmm3 is compared with next. The collision state is in r15.
constexpr Hooks::Entry Reach{0x155f037, 8, {0xc5, 0xfa, 0x58, 0x05, 0x69, 0x6e, 0x36, 0x00}};
constexpr VAddr ReachNext = 0x155f03f;

// The frame times in seconds, one float per time group; the state names its group at +0x44.
constexpr VAddr FrameTimes = 0x1efb100;
constexpr u32 StateTimeGroup = 0x44;

// The game's 0.1 m in a frame of 1/30 s.
constexpr float ReachOfFrame = 0.1f;
constexpr float ReachPerSecond = 3.0f;

// The stub below takes about 70 bytes.
constexpr size_t StubBytes = 128;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the camera obstacle test's hook");
        return false;
    }
    Xbyak::Label per_second;
    Xbyak::Label of_frame;
    const void* stub = c.getCurr();
    c.push(rcx);
    c.push(rdx);
    c.lea(rsp, ptr[rsp - 16]);
    c.vmovups(ptr[rsp], xmm1);
    c.mov(rcx, module_base + FrameTimes - GoldHenBase);
    c.mov(edx, dword[r15 + StateTimeGroup]);
    c.vmovss(xmm1, dword[rcx + rdx * 4]);
    c.vmulss(xmm1, xmm1, dword[rip + per_second]);
    c.vminss(xmm1, xmm1, dword[rip + of_frame]);
    c.vaddss(xmm0, xmm0, xmm1);
    c.vmovups(xmm1, ptr[rsp]);
    c.lea(rsp, ptr[rsp + 16]);
    c.pop(rdx);
    c.pop(rcx);
    c.jmp(reinterpret_cast<const void*>(module_base + ReachNext - GoldHenBase));
    c.L(per_second);
    c.dd(std::bit_cast<u32>(ReachPerSecond));
    c.L(of_frame);
    c.dd(std::bit_cast<u32>(ReachOfFrame));
    if (Hooks::Replace(module_base, Reach, c, capacity, stub) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera's obstacle test is not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::CameraReach
