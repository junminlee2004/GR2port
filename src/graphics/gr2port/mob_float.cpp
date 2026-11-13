// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/mob_float.h"

namespace Gr2Port::MobFloat {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float: the one the people's clocks advance by.
constexpr VAddr StepTime = 0x1efb104;

// FUN_0109cb20: vmulss xmm0, xmm0, [0x18c1d7c] makes the sway's angle, the sum of two sines times
// 0.1 rad. The game writes xmm1 before it reads it again.
constexpr Hooks::Entry Sway{0x109cd07, 8, {0xc5, 0xfa, 0x59, 0x05, 0x6d, 0x50, 0x82, 0x00}};
constexpr VAddr SwayNext = 0x109cd0f;
constexpr VAddr SwayAngle = 0x18c1d7c;

// FUN_0109d030: vmovaps xmm8, [0x188f990] loads the half angle, 0.0375 rad in each lane, of the
// turn a person in the field for half a second gets each frame. The game writes xmm3 next.
constexpr Hooks::Entry FieldTurn{0x109d18d, 8, {0xc5, 0x78, 0x28, 0x05, 0xfb, 0x27, 0x7f, 0x00}};
constexpr VAddr FieldTurnNext = 0x109d195;
constexpr VAddr FieldTurnAngle = 0x188f990;

// FUN_00c02370: vbroadcastss xmm0, [rbx + 0x40] loads the angle the task turns its person by
// each frame.
constexpr Hooks::Entry Tumble{0xc02769, 6, {0xc4, 0xe2, 0x79, 0x18, 0x43, 0x40}};
constexpr VAddr TumbleNext = 0xc0276f;

// FUN_00be33b0, with the task in r13 and its tilt [r13 + 0x30] in xmm0: the instructions from
// vmulss xmm1, xmm0, [0x18bbe98] up to TiltNext make the tilt fade, 0.99 of it a frame, move the
// angle [r13 + 0x2c] a tenth of the way to it, and leave the angle in xmm0 for the turn. The
// game writes xmm1 to xmm4 before it reads them again.
constexpr Hooks::Entry Tilt{0xbe3989, 8, {0xc5, 0xfa, 0x59, 0x0d, 0x07, 0x85, 0xcd, 0x00}};
constexpr VAddr TiltNext = 0xbe39b7;
constexpr VAddr TiltFade = 0x18bbe98;  // 0.99
constexpr VAddr AngleKeep = 0x18bbe9c; // 0.9

// An instruction of a float task that adds an amount to a height in an xmm register once a
// frame, or takes it away. The amount is a constant, or a value of the task.
struct Step {
    Hooks::Entry entry;
    /// The number of the height's register.
    u8 height;
    bool takes_away;
    /// The constant's address, or 0 with the number of the task's register and the offset of
    /// the amount in the task.
    VAddr amount;
    u8 task;
    u8 offset;
};

constexpr std::array Steps{
    // FUN_0109cb20: the person rises 0.03 a frame up to the task's limit, and falls 0.1 a frame
    // once Kat lets go.
    Step{{0x109cc68, 8, {0xc5, 0xfa, 0x58, 0x05, 0xf8, 0x50, 0x82, 0x00}}, 0, false, 0x18c1d68},
    Step{{0x109cf4b, 8, {0xc5, 0xfa, 0x58, 0x05, 0x19, 0x4e, 0x82, 0x00}}, 0, false, 0x18c1d6c},
    // FUN_00be33b0: the person rises 0.03 a frame. Let go, the person falls by the speed
    // [r13 + 0x44], the bob loses 0.03 and the speed gains 0.03, each a frame.
    Step{{0xbe38f7, 8, {0xc5, 0xf2, 0x58, 0x0d, 0x71, 0x85, 0xcd, 0x00}}, 1, false, 0x18bbe70},
    Step{{0xbe35ca, 6, {0xc4, 0xc1, 0x7a, 0x5c, 0x45, 0x44}}, 0, true, 0, 13, 0x44},
    Step{{0xbe35e0, 8, {0xc5, 0xf2, 0x58, 0x0d, 0x80, 0x88, 0xcd, 0x00}}, 1, false, 0x18bbe68},
    Step{{0xbe3719, 8, {0xc5, 0xfa, 0x58, 0x05, 0x4f, 0x87, 0xcd, 0x00}}, 0, false, 0x18bbe70},
    // FUN_00c02370: the same, with the speed in [rbx + 0x3c].
    Step{{0xc02567, 8, {0xc5, 0xfa, 0x58, 0x05, 0x49, 0x9d, 0xcb, 0x00}}, 0, false, 0x18bc2b8},
    Step{{0xc025e9, 5, {0xc5, 0xfa, 0x5c, 0x43, 0x3c}}, 0, true, 0, 3, 0x3c},
    Step{{0xc025fc, 8, {0xc5, 0xf2, 0x58, 0x0d, 0xc4, 0x9c, 0xcb, 0x00}}, 1, false, 0x18bc2c8},
    Step{{0xc0275c, 8, {0xc5, 0xfa, 0x58, 0x05, 0x54, 0x9b, 0xcb, 0x00}}, 0, false, 0x18bc2b8},
};

// The stack bytes a step's stub moves down by to keep xmm15: the red zone and the register.
constexpr u32 Spill = 0xa0;

// The frames per second the angles and the amounts are made for.
constexpr float FrameRate = 30.0f;
// The natural logarithms of 0.99 and 0.9.
constexpr float LogTiltFade = -0.010050336f;
constexpr float LogAngleKeep = -0.10536052f;
// The longest step, in 30 fps frames, the tilt's stub raises 0.99 and 0.9 to.
constexpr float LongestStep = 4.0f;

// The stubs and their constants.
constexpr size_t StubBytes = 1536;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) {
        return reinterpret_cast<const u8*>(module_base + address - GoldHenBase);
    };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the floating people's hooks");
        return false;
    }
    Xbyak::Label frame_rate, one, half, sixth, log_tilt_fade, log_angle_keep, longest_step;
    const auto hook = [&](const Hooks::Entry& entry, const void* stub, const char* task) {
        if (Hooks::Replace(module_base, entry, c, capacity, stub) != Hooks::Result::Hooked) {
            LOG_ERROR(Render_Vulkan, "GR2port: the floating people's {} is not the expected code",
                      task);
            return false;
        }
        return true;
    };
    // The stubs leave the flags alone: the game tests them after the sway's instruction.
    const void* sway = c.getCurr();
    c.vmulss(xmm0, xmm0, dword[rip + code(SwayAngle)]);
    c.vmovss(xmm1, dword[rip + code(StepTime)]);
    c.vmulss(xmm1, xmm1, dword[rip + frame_rate]);
    c.vmulss(xmm0, xmm0, xmm1);
    c.jmp(code(SwayNext));

    const void* field_turn = c.getCurr();
    c.vbroadcastss(xmm8, dword[rip + code(StepTime)]);
    c.vbroadcastss(xmm3, dword[rip + frame_rate]);
    c.vmulps(xmm8, xmm8, xmm3);
    c.vmulps(xmm8, xmm8, xword[rip + code(FieldTurnAngle)]);
    c.jmp(code(FieldTurnNext));

    const void* tumble = c.getCurr();
    c.vmovss(xmm0, dword[rbx + 0x40]);
    c.vmulss(xmm0, xmm0, dword[rip + code(StepTime)]);
    c.vmulss(xmm0, xmm0, dword[rip + frame_rate]);
    c.vshufps(xmm0, xmm0, xmm0, 0);
    c.jmp(code(TumbleNext));

    // With f the step's time in 30 fps frames, the tilt keeps 0.99^f of itself and the angle
    // 0.9^f, and the turn is f of the angle. A power k^f is k * e^((f - 1) * ln k), with the
    // series of e^x up to x^2 for 0.99 and up to x^3 for 0.9: at 30 fps both are the game's own.
    const void* tilt = c.getCurr();
    c.vmovss(xmm3, dword[rip + code(StepTime)]);
    c.vmulss(xmm3, xmm3, dword[rip + frame_rate]);
    c.vminss(xmm1, xmm3, dword[rip + longest_step]);
    c.vsubss(xmm1, xmm1, dword[rip + one]);
    c.vmulss(xmm2, xmm1, dword[rip + log_tilt_fade]);
    c.vmulss(xmm4, xmm2, dword[rip + half]);
    c.vaddss(xmm4, xmm4, dword[rip + one]);
    c.vmulss(xmm4, xmm4, xmm2);
    c.vaddss(xmm4, xmm4, dword[rip + one]);
    c.vmulss(xmm4, xmm4, dword[rip + code(TiltFade)]);
    c.vmulss(xmm4, xmm4, xmm0);
    c.vmovss(dword[r13 + 0x30], xmm4);
    c.vmulss(xmm2, xmm1, dword[rip + log_angle_keep]);
    c.vmulss(xmm4, xmm2, dword[rip + sixth]);
    c.vaddss(xmm4, xmm4, dword[rip + half]);
    c.vmulss(xmm4, xmm4, xmm2);
    c.vaddss(xmm4, xmm4, dword[rip + one]);
    c.vmulss(xmm4, xmm4, xmm2);
    c.vaddss(xmm4, xmm4, dword[rip + one]);
    c.vmulss(xmm4, xmm4, dword[rip + code(AngleKeep)]);
    // The angle moves to 0.99 of the tilt, as the game's 0.9 and 0.099 make it.
    c.vmovss(xmm1, dword[rip + one]);
    c.vsubss(xmm1, xmm1, xmm4);
    c.vmulss(xmm1, xmm1, dword[rip + code(TiltFade)]);
    c.vmulss(xmm1, xmm1, xmm0);
    c.vmulss(xmm4, xmm4, dword[r13 + 0x2c]);
    c.vaddss(xmm0, xmm4, xmm1);
    c.vmovss(dword[r13 + 0x2c], xmm0);
    c.vmulss(xmm0, xmm0, xmm3);
    c.jmp(code(TiltNext));

    // A step's stub adds the amount times the step's time in 30 fps frames. It keeps xmm15,
    // which the game may hold a value in, below the stack's red zone.
    std::array<const void*, Steps.size()> steps;
    for (size_t i = 0; i < Steps.size(); i++) {
        const Step& step = Steps[i];
        const Xbyak::Xmm height{step.height};
        steps[i] = c.getCurr();
        c.lea(rsp, ptr[rsp - Spill]);
        c.vmovups(ptr[rsp], ymm15);
        c.vmovss(xmm15, dword[rip + code(StepTime)]);
        c.vmulss(xmm15, xmm15, dword[rip + frame_rate]);
        if (step.amount != 0) {
            c.vmulss(xmm15, xmm15, dword[rip + code(step.amount)]);
        } else {
            c.vmulss(xmm15, xmm15, dword[Xbyak::Reg64{step.task} + step.offset]);
        }
        if (step.takes_away) {
            c.vsubss(height, height, xmm15);
        } else {
            c.vaddss(height, height, xmm15);
        }
        c.vmovups(ymm15, ptr[rsp]);
        c.lea(rsp, ptr[rsp + Spill]);
        c.jmp(code(step.entry.address + step.entry.size));
    }

    const auto real = [&c](Xbyak::Label& label, float value) {
        c.L(label);
        c.dd(std::bit_cast<u32>(value));
    };
    real(frame_rate, FrameRate);
    real(one, 1.0f);
    real(half, 0.5f);
    real(sixth, 1.0f / 6.0f);
    real(log_tilt_fade, LogTiltFade);
    real(log_angle_keep, LogAngleKeep);
    real(longest_step, LongestStep);
    if (!hook(Sway, sway, "float task") || !hook(FieldTurn, field_turn, "field task") ||
        !hook(Tumble, tumble, "tumble task") || !hook(Tilt, tilt, "tilt task")) {
        return false;
    }
    for (size_t i = 0; i < Steps.size(); i++) {
        if (!hook(Steps[i].entry, steps[i], "rise or fall")) {
            return false;
        }
    }
    return true;
}

} // namespace Gr2Port::MobFloat
