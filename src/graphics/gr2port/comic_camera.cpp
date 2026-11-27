// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/comic_camera.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::ComicCamera {

namespace {

// The real length of this frame in 30 fps frames, at most 1, a float. Slow motion and a stopped
// game clock leave it alone.
constexpr VAddr FrameShare = 0x1f7c600;

// FUN_017742c0, a 3D comic page's update, with the page in rbx and the frames since the panel
// change in r12d: mov edx, [rbx + 0x2e4], mov eax, [rbx + 0x2e8] and mov ecx, [rbx + 0x2ec] load
// the frames before the fractions grow, the frames the angles wait and the frames the growth
// takes. The game writes xmm2 and xmm3 before it reads them again.
constexpr Hooks::Entry Counts{0x177585c, 6, {0x8b, 0x93, 0xe4, 0x02, 0x00, 0x00}};
constexpr std::array<u8, 12> LaterCounts{0x8b, 0x83, 0xe8, 0x02, 0x00, 0x00,
                                         0x8b, 0x8b, 0xec, 0x02, 0x00, 0x00};
constexpr VAddr CountsNext = 0x177586e;
constexpr u32 FramesBeforeGrowth = 0x2e4;
constexpr u32 AnglesWait = 0x2e8;
constexpr u32 GrowthFrames = 0x2ec;

// vpermilps xmm2, xmm7, 0 spreads the fraction of the way the camera's points move this frame,
// and vpermilps xmm1, xmm11, 0 the fraction its angles turn. The game later compares xmm11 with
// the page's threshold that starts the panel's animation.
constexpr Hooks::Entry Move{0x1775afd, 6, {0xc4, 0xe3, 0x79, 0x04, 0xd7, 0x00}};
constexpr VAddr MoveNext = 0x1775b03;
constexpr Hooks::Entry Turn{0x1775bc4, 6, {0xc4, 0xc3, 0x79, 0x04, 0xcb, 0x00}};
constexpr VAddr TurnNext = 0x1775bca;

// After the steps the game keeps three gaps between the camera and the panel in stack slots, in
// the first float of each: two of the camera's points and one of its angles. Gaps under 1, 1 and
// 10 start the panel's animation, and gaps under 0.00001 end the move once no animation plays.
// vmovaps xmm0, [rbp - 0x1c0] follows the last of the three stores, and the game writes rax and
// xmm0 before it reads them again.
constexpr Hooks::Entry Gaps{0x17760a0, 8, {0xc5, 0xf8, 0x28, 0x85, 0x40, 0xfe, 0xff, 0xff}};
constexpr VAddr GapsNext = 0x17760a8;
constexpr std::array<s32, 2> PointGaps{-0x230, -0x240};
constexpr s32 AngleGap = -0x250;
constexpr s32 TurnFraction = -0x1c0;

// FUN_017772a0, a 3D comic's update, with the comic's menu in r12. On the last page,
// mov ecx, [r12 + 0xa0] loads the frames left to wait, 20 in a new menu, and the comic can end once
// they run out and no animation plays. The call before it leaves no vector register live, and the
// game writes rax and rdx right after it.
constexpr Hooks::Entry LastPageWait{0x1778382, 8, {0x41, 0x8b, 0x8c, 0x24, 0xa0, 0x00, 0x00, 0x00}};
constexpr u32 LastPageFrames = 0xa0;
// The game's own count of the wait, and the instruction that takes the wait's result from al.
constexpr VAddr LastPageCount = 0x177838a;
constexpr VAddr LastPageResult = 0x17783a2;

// The fewest 30 fps frames a step's time is taken as.
constexpr float ShortestStep = 1.0f / 64.0f;

// The stack bytes a stub moves down by first: the red zone.
constexpr s32 RedZone = 128;

// The stubs and their constants.
constexpr size_t StubBytes = 1024;

VAddr g_module_base = 0;

// The real time the last page's wait holds toward its next 30 fps frame.
float g_last_page_time = 0.0f;

// What the rest of a 30 fps frame's step would leave of the gap this frame's step leaves, for the
// camera's points and for its angles. The gaps' stub uses both and sets them back to 1.
struct Rests {
    float points;
    float angles;
};
Rests g_rests{1.0f, 1.0f};

// The frame's time in 30 fps frames, between ShortestStep and 1.
float StepFrames() {
    float frames;
    std::memcpy(&frames, Hooks::Code(g_module_base, FrameShare), sizeof(frames));
    return frames > ShortestStep ? frames : ShortestStep;
}

// The fraction of the way the step moves when a 30 fps frame moves fraction of it:
// 1 - (1 - fraction)^f, with f the step's time in 30 fps frames. At 30 fps it is fraction itself.
// rest then gets (1 - fraction)^(1 - f), which takes the gap this step leaves to the gap a 30 fps
// frame's step leaves.
float PS4_SYSV_ABI StepFraction(float fraction, float* rest) {
    const float frames = StepFrames();
    if (frames >= 1.0f || !(fraction > 0.0f) || fraction >= 1.0f) {
        return fraction;
    }
    const float left = std::pow(1.0f - fraction, frames);
    *rest = (1.0f - fraction) / left;
    return 1.0f - left;
}

// Writes a stub that calls StepFraction with the low float of from and with rest, leaves the
// result in each lane of to, as vpermilps would, and jumps to next. It keeps the flags and every
// other register.
const void* FractionStub(Xbyak::CodeGenerator& c, const Xbyak::Xmm& from, const Xbyak::Xmm& to,
                         float* rest, const u8* next) {
    using namespace Xbyak::util;
    const std::array<Xbyak::Reg64, 9> saved{rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11};
    constexpr u32 VectorRegisters = 16;
    constexpr u32 VectorBytes = 32;
    const void* stub = c.getCurr();
    c.lea(rsp, ptr[rsp - RedZone]);
    c.pushfq();
    for (const auto& reg : saved) {
        c.push(reg);
    }
    c.push(rbp);
    c.mov(rbp, rsp);
    c.sub(rsp, VectorRegisters * VectorBytes);
    c.and_(rsp, ~u32{VectorBytes - 1});
    for (u32 i = 0; i < VectorRegisters; i++) {
        c.vmovdqu(ptr[rsp + i * VectorBytes], Xbyak::Ymm(static_cast<int>(i)));
    }
    c.vmovaps(xmm0, from);
    c.mov(rdi, reinterpret_cast<u64>(rest));
    c.mov(rax, reinterpret_cast<u64>(&StepFraction));
    c.call(rax);
    c.vshufps(xmm0, xmm0, xmm0, 0);
    c.vmovdqu(ptr[rsp + to.getIdx() * VectorBytes], ymm0);
    for (u32 i = 0; i < VectorRegisters; i++) {
        c.vmovdqu(Xbyak::Ymm(static_cast<int>(i)), ptr[rsp + i * VectorBytes]);
    }
    c.mov(rsp, rbp);
    c.pop(rbp);
    for (auto reg = saved.rbegin(); reg != saved.rend(); ++reg) {
        c.pop(*reg);
    }
    c.popfq();
    c.lea(rsp, ptr[rsp + RedZone]);
    c.jmp(next);
    return stub;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    g_module_base = module_base;
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the comic camera's hooks");
        return false;
    }
    if (std::memcmp(code(Counts.address + Counts.size), LaterCounts.data(), LaterCounts.size()) !=
        0) {
        LOG_ERROR(Render_Vulkan, "GR2port: the comic camera's counts are not the expected code");
        return false;
    }
    Xbyak::Label one, shortest_step;

    // Each count of 30 fps frames becomes the count of steps that take as long.
    const void* counts = c.getCurr();
    c.vmovss(xmm2, dword[rip + code(FrameShare)]);
    c.vmaxss(xmm2, xmm2, dword[rip + shortest_step]);
    c.vmovss(xmm3, dword[rip + one]);
    c.vdivss(xmm3, xmm3, xmm2);
    const std::array<std::pair<Xbyak::Reg32, u32>, 3> loads{std::pair{edx, FramesBeforeGrowth},
                                                            std::pair{eax, AnglesWait},
                                                            std::pair{ecx, GrowthFrames}};
    for (const auto& [reg, offset] : loads) {
        c.vcvtsi2ss(xmm2, xmm2, dword[rbx + offset]);
        c.vmulss(xmm2, xmm2, xmm3);
        c.vcvtss2si(reg, xmm2);
    }
    c.jmp(code(CountsNext));
    c.L(one);
    c.dd(std::bit_cast<u32>(1.0f));
    c.L(shortest_step);
    c.dd(std::bit_cast<u32>(ShortestStep));

    const void* move = FractionStub(c, xmm7, xmm2, &g_rests.points, code(MoveNext));
    const void* turn = FractionStub(c, xmm11, xmm1, &g_rests.angles, code(TurnNext));

    // The game's tests see the gaps a 30 fps frame's step would leave, so a move's first frame
    // passes them when it does at 30 fps.
    const void* gaps = c.getCurr();
    c.mov(rax, reinterpret_cast<u64>(&g_rests));
    for (const s32 gap : PointGaps) {
        c.vmovss(xmm0, dword[rbp + gap]);
        c.vmulss(xmm0, xmm0, dword[rax + offsetof(Rests, points)]);
        c.vmovss(dword[rbp + gap], xmm0);
    }
    c.vmovss(xmm0, dword[rbp + AngleGap]);
    c.vmulss(xmm0, xmm0, dword[rax + offsetof(Rests, angles)]);
    c.vmovss(dword[rbp + AngleGap], xmm0);
    c.mov(dword[rax + offsetof(Rests, points)], std::bit_cast<u32>(1.0f));
    c.mov(dword[rax + offsetof(Rests, angles)], std::bit_cast<u32>(1.0f));
    c.vmovaps(xmm0, ptr[rbp + TurnFraction]);
    c.jmp(code(GapsNext));

    // The wait's count drops once per 30 fps frame of real time. A frame that does not complete
    // one reports the wait as still running.
    Xbyak::Label whole_frame, game_count;
    const void* last_page = c.getCurr();
    c.mov(ecx, dword[r12 + LastPageFrames]);
    c.test(ecx, ecx);
    c.jle(game_count);
    c.mov(rax, reinterpret_cast<u64>(&g_last_page_time));
    c.vmovss(xmm0, dword[rax]);
    c.vaddss(xmm0, xmm0, dword[rip + code(FrameShare)]);
    c.vcomiss(xmm0, dword[rip + one]);
    c.jae(whole_frame);
    c.vmovss(dword[rax], xmm0);
    c.xor_(eax, eax);
    c.jmp(code(LastPageResult));
    c.L(whole_frame);
    c.vsubss(xmm0, xmm0, dword[rip + one]);
    c.vmovss(dword[rax], xmm0);
    c.L(game_count);
    c.jmp(code(LastPageCount));

    if (Hooks::Replace(module_base, Counts, c, capacity, counts) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, Move, c, capacity, move) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, Turn, c, capacity, turn) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, Gaps, c, capacity, gaps) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, LastPageWait, c, capacity, last_page) !=
            Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the comic camera's move is not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::ComicCamera
