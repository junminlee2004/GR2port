// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numbers>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hair_flutter.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::HairFlutter {

namespace {

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// FUN_01501b20, with the air's mean speed along the chain in xmm0: vsubss xmm1, xmm0,
// [rbx + 0xa8] subtracts the chain's limit, the speed in a frame above which the hair flutters.
constexpr Hooks::Entry Limit{0x1501f08, 8, {0xc5, 0xfa, 0x5c, 0x8b, 0xa8, 0x00, 0x00, 0x00}};
constexpr VAddr LimitNext = 0x1501f10;

// FUN_01501b20, with the excess in xmm1 and the chain in rbx: vmulss xmm1, xmm1, [0x18c5804]
// multiplies the excess by 0.4 into the size of the wave's push on the middle node.
constexpr Hooks::Entry Size{0x1502307, 8, {0xc5, 0xf2, 0x59, 0x0d, 0xf5, 0x34, 0x3c, 0x00}};
constexpr VAddr SizeNext = 0x150230f;
constexpr float SizeFactor = 0.4f;

// The chain: its node count, its nodes' drags and its wave's rate. A step advances the wave's
// phase by the excess times PhaseFactor times the rate, in cycles.
constexpr u32 ChainNodes = 0x40;
constexpr u32 ChainDrags = 0x80;
constexpr u32 ChainRate = 0xb0;
constexpr float PhaseFactor = 0.8333334f;

// FUN_01590f20, while Kat floats: vmovss xmm0, [0x18c6904] loads 0.045, the air that
// FUN_01523db0 then pushes along her hair in each frame.
constexpr Hooks::Entry FloatPush{0x15931d8, 8, {0xc5, 0xfa, 0x10, 0x05, 0x24, 0x37, 0x33, 0x00}};
constexpr VAddr FloatPushNext = 0x15931e0;
constexpr VAddr FloatPushAmount = 0x18c6904;

// FUN_008f6430, while Kat + 0x5464 is set and Kat + 0x5400 is 0, with Kat's hair's root in rax:
// vaddps xmm0, xmm0, [rax + 0x70]; vmovaps [rax + 0x70], xmm0 adds to the root's pushes the air
// in xmm0, (0.01 cos, -0.02 - 0.03 sin, 0.01 sin) of a phase that the fps patch turns by the
// frame's time.
constexpr Hooks::Entry Swirl{
    0x8fadba, 10, {0xc5, 0xf8, 0x58, 0x40, 0x70, 0xc5, 0xf8, 0x29, 0x40, 0x70}};
constexpr VAddr SwirlNext = 0x8fadc4;

// FUN_008f6430 next, with the root's pushes in xmm0: in Jupiter style vaddps xmm0, xmm0,
// [0x1845cc0] adds (0, -0.03, 0, 0) and in Lunar style vaddps xmm0, xmm0, [0x1845cd0] adds
// (0, 0.015, 0, 0) in each frame. Both go on to vmovaps [rax + 0x70], xmm0, and xmm1 is written
// before it is read.
constexpr Hooks::Entry JupiterPush{0x8faeef, 8, {0xc5, 0xf8, 0x58, 0x05, 0xc9, 0xad, 0xf4, 0x00}};
constexpr VAddr JupiterPushNext = 0x8faef7;
constexpr VAddr JupiterPushAmount = 0x1845cc0;
constexpr Hooks::Entry LunarPush{0x8faf6f, 8, {0xc5, 0xf8, 0x58, 0x05, 0x59, 0xad, 0xf4, 0x00}};
constexpr VAddr LunarPushNext = 0x8faf77;
constexpr VAddr LunarPushAmount = 0x1845cd0;

// FUN_008f6430 next, while objects are in Kat's stasis field, with her hair's root in rax:
// vaddps xmm0, xmm0, [rax + 0x70]; vmovaps [rax + 0x70], xmm0 adds her up axis times 0.03 and a
// share of the objects' motion in each frame. xmm1 is written before it is read.
constexpr Hooks::Entry FieldPush{
    0x8fb417, 10, {0xc5, 0xf8, 0x58, 0x40, 0x70, 0xc5, 0xf8, 0x29, 0x40, 0x70}};
constexpr VAddr FieldPushNext = 0x8fb421;

// FUN_008f6430 next, with the step's time in xmm0 and her style's gravity per second in xmm1:
// vpermilps xmm0, xmm0, 0 broadcasts the time, and the game stores the product as the gravity of
// Kat's hair's root. The game does not read the product again.
constexpr Hooks::Entry StyleGravity{0x8fbac2, 6, {0xc4, 0xe3, 0x79, 0x04, 0xc0, 0x00}};
constexpr VAddr StyleGravityNext = 0x8fbac8;

// 1/30 in all four lanes.
constexpr VAddr FrameTime = 0x18a0c60;

// FUN_01506ac0, a root's update: vmovaps xmm0, [r14 + 0x50] loads the gravity that the root's
// chains add to their speeds in a step. Other characters' roots keep a constant for a 30 fps
// frame, so every root stores a 30 fps frame's gravity and this takes the step's share of it.
constexpr Hooks::Entry RootGravity{0x1506ae5, 6, {0xc4, 0xc1, 0x78, 0x28, 0x46, 0x50}};
constexpr VAddr RootGravityNext = 0x1506aeb;

// FUN_014f2280, the step of a dress's chains: vmovaps xmm0, [rdi + 0x80] loads the gravity they
// add to their speeds. A step's share of it is too light to hold them down and they whip around
// above 30 fps, so they take the root's gravity for a 30 fps frame in every step.
constexpr Hooks::Entry DressGravity{0x14f2847, 8, {0xc5, 0xf8, 0x28, 0x87, 0x80, 0x00, 0x00, 0x00}};
constexpr VAddr DressGravityNext = 0x14f284f;

// FUN_014f66b0(chain), the step of strands such as a braid or twin tails. A node keeps its speed,
// gains the root's gravity, is pulled towards its animated place and loses speed to drag, by rates
// made for a 30 fps frame. StrandStep runs the step with the rates for the step's time.
constexpr Hooks::Entry StrandEntry{0x14f66b0, 6, Hooks::FramePrologue};
// The step's call of FUN_01509a20(speed, size, waves, push), which adds the waves to the push on
// one node and advances their phases.
constexpr Hooks::Entry StrandWaveCall{0x14f7181, 5, {0xe8, 0x9a, 0x28, 0x01, 0x00}};
constexpr VAddr StrandWaveNext = 0x14f7186;
constexpr VAddr StrandWaveFunction = 0x1509a20;

// A strand's fields. With StrandLatePull set, its pull follows the drag and runs once in each of
// the step's passes and once after them.
constexpr u32 StrandRoot = 0x20;
constexpr u32 RootAir = 0x90;
constexpr u32 StrandFlags = 0x30;
constexpr s32 StrandLatePull = 0x10;
constexpr u32 StrandNodes = 0x40;
constexpr u32 StrandPasses = 0x44;
constexpr u32 StrandMorePasses = 0x48;
constexpr u32 StrandShortRatio = 0x4c;
constexpr u32 StrandPlaces = 0x80;
constexpr u32 StrandSpeeds = 0x88;
constexpr u32 StrandRests = 0xa0;
constexpr u32 StrandGravities = 0xa8;
constexpr u32 StrandDrags = 0xb0;
constexpr u32 StrandBends = 0xc8;
constexpr u32 StrandPulls = 0xe8;
constexpr u32 StrandWaveLimit = 0x10c;
constexpr u32 StrandWaveNode = 0x110;
// From the waves, which are the strand's field 0x108.
constexpr u32 WavesTable = 0x10;
constexpr s32 StrandMostNodes = 64;
constexpr float SmallestRate = 1e-5f;
// The share of a node's move in KeepLinkLengths that its parent takes in the opposite direction.
constexpr float LinkReaction = 0.9f;
// The share of the speed lost to a 30 fps step's straight moves that SlowLinkTurns takes.
constexpr float LinkTurnShare = 0.25f;

// The step's load of a node's drag: vbroadcastss xmm3, [rax]. A stub first adds the node's offset
// to the place in xmm1 that the drag acts from.
constexpr Hooks::Entry StrandDragLoad{0x14f6b99, 5, {0xc4, 0xe2, 0x79, 0x18, 0x18}};
constexpr VAddr StrandDragNext = 0x14f6b9e;

// What the strand's drag pointer holds during its step: each node's drag and, fixed distances past
// it, the x, y and z of the offset that the stub adds.
struct StrandDragRates {
    std::array<float, StrandMostNodes> drags;
    std::array<std::array<float, StrandMostNodes>, 3> offsets;
};
constexpr u32 StrandOffsets = offsetof(StrandDragRates, offsets);
constexpr u32 StrandOffsetStride = sizeof(std::array<float, StrandMostNodes>);

struct StrandWaveEntry {
    float reference;
    float phase;
    float limit;
    float rate;
    float excess_rate;
    float size;
    float excess_size;
};
static_assert(sizeof(StrandWaveEntry) == 0x1c);

using StrandStepFunction = void PS4_SYSV_ABI (*)(u8* chain);
using StrandWaveFunctionType = void PS4_SYSV_ABI (*)(float speed, float size, u8* waves,
                                                     float* push);

// The frames per second the limit and the pushes are made for.
constexpr float GameRate = 30.0f;

// The stack bytes a stub moves down by first: the red zone.
constexpr s32 RedZone = 128;

// The twelve stubs, their constant and the strand step's trampoline.
constexpr size_t StubBytes = 940;

VAddr g_module_base = 0;
StrandStepFunction g_strand_step = nullptr;
// How much the running strand step scales the push of its waves.
thread_local float t_strand_wave_push = 1.0f;

// A node keeps 1 - drag of its speed in each step, gains the step's push and moves by its speed.
// For a push that turns by omega in each step, the node's swing is 1 / Response(drag, omega)
// times the push.
float Response(float drag, float omega) {
    const float keep = 1.0f - drag;
    const float cosine = std::cos(omega);
    return std::sqrt((1.0f - 2.0f * keep * cosine + keep * keep) * (1.0f - cosine));
}

// The size of the wave's push on chain's middle node for an excess of excess. Above 30 fps the
// air, the limit and so the excess are the step's share of a 30 fps frame's, and the steps keep
// the stock drag, so the wave that a 30 fps step barely follows moves the hair far more. The
// push takes the ratio of the swing at 30 fps to the swing at this step's time, for the wave's
// turn in a 30 fps frame and the middle node's drag, and never grows past the plain size.
float PS4_SYSV_ABI FlutterSize(float excess, const u8* chain) {
    const float size = excess * SizeFactor;
    float step;
    std::memcpy(&step, Hooks::Code(g_module_base, StepTime), sizeof(step));
    const float frames = step * GameRate;
    if (!(frames > 0.0f) || frames >= 1.0f || !(excess > 0.0f)) {
        return size;
    }
    s32 nodes;
    const u8* drags;
    float rate;
    float drag;
    std::memcpy(&nodes, chain + ChainNodes, sizeof(nodes));
    std::memcpy(&drags, chain + ChainDrags, sizeof(drags));
    std::memcpy(&rate, chain + ChainRate, sizeof(rate));
    std::memcpy(&drag, drags + ((nodes + 1) >> 1) * sizeof(drag), sizeof(drag));
    const float turn = 2.0f * std::numbers::pi_v<float> * excess * PhaseFactor * rate / frames;
    const float ratio = Response(drag, turn * frames) / (frames * Response(drag, turn));
    return ratio < 1.0f ? size * ratio : size;
}

// Writes a stub that sets the low lane of xmm1 to FlutterSize of it and of the chain in rbx, and
// jumps to next. It keeps the flags and every other register.
const void* SizeStub(Xbyak::CodeGenerator& c, const u8* next) {
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
    c.vmovaps(xmm0, xmm1);
    c.mov(rdi, rbx);
    c.mov(rax, reinterpret_cast<u64>(&FlutterSize));
    c.call(rax);
    c.vmovss(dword[rsp + 1 * VectorBytes], xmm0);
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

float StepFrames() {
    float step;
    std::memcpy(&step, Hooks::Code(g_module_base, StepTime), sizeof(step));
    return step * GameRate;
}

template <typename T>
T Field(const u8* object, u32 offset) {
    T value;
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}

template <typename T>
void SetField(u8* object, u32 offset, T value) {
    std::memcpy(object + offset, &value, sizeof(value));
}

struct StrandRates {
    float gravity;
    float drag;
    float pull;
    // What a push on the node is scaled by.
    float push;
    // The share of the air's speed past the root that the node's drag acts from beyond its place.
    float air;
};

// A node's distance x from its animated place follows x' = x + carried * speed - spring * x + push
// in a 30 fps frame. The two roots of that recurrence, raised to the step's share of a frame, give
// the carried and spring of a step whose motion passes through the same places at each 30 fps
// frame. late_pulls is the number of pulls after the drag, or 0 for one pull before it.
StrandRates RatesForStep(float gravity, float drag, float pull, s32 late_pulls, float frames) {
    const float keep = 1.0f - drag;
    const float unpulled =
        late_pulls > 0 ? std::pow(1.0f - pull, static_cast<float>(late_pulls)) : 1.0f - pull;
    const float carried = keep * unpulled;
    const float spring = late_pulls > 0 ? 1.0f - unpulled : keep * pull;
    const float carried_step = std::pow(carried, frames);
    const float sum = 1.0f + carried - spring;
    const float discriminant = sum * sum - 4.0f * carried;
    float powers;
    if (discriminant >= 0.0f) {
        const float larger = 0.5f * (sum + std::sqrt(discriminant));
        const float smaller = larger > 0.0f ? carried / larger : 0.0f;
        powers = std::pow(larger, frames) + std::pow(smaller, frames);
    } else {
        const float radius = std::sqrt(carried);
        const float angle = std::acos(std::clamp(0.5f * sum / radius, -1.0f, 1.0f));
        powers = 2.0f * std::pow(radius, frames) * std::cos(frames * angle);
    }
    const float spring_step =
        pull == 0.0f ? 0.0f : std::clamp(1.0f + carried_step - powers, 0.0f, 1.0f);

    StrandRates rates{};
    if (spring > SmallestRate) {
        rates.push = spring_step / spring;
    } else if (carried < 1.0f - SmallestRate) {
        rates.push = frames * (1.0f - carried_step) / (1.0f - carried);
    } else {
        rates.push = frames * frames;
    }
    // The root's gravity is already the step's share of a 30 fps frame's.
    rates.gravity =
        carried_step > 0.0f ? gravity * rates.push * carried / (carried_step * frames) : 0.0f;
    if (late_pulls > 0) {
        const float left = 1.0f - spring_step;
        rates.drag = left > 0.0f ? std::clamp(1.0f - carried_step / left, 0.0f, 1.0f) : drag;
        rates.pull =
            left > 0.0f ? 1.0f - std::pow(left, 1.0f / static_cast<float>(late_pulls)) : pull;
        const float taken = left * rates.drag;
        rates.air = taken > 0.0f ? rates.push * unpulled * drag / (frames * taken) - 1.0f : 0.0f;
    } else {
        const float kept = carried_step + spring_step;
        rates.drag = std::clamp(1.0f - kept, 0.0f, 1.0f);
        rates.pull = kept > 0.0f ? spring_step / kept : pull;
        rates.air = rates.drag > 0.0f ? rates.push * drag / (frames * rates.drag) - 1.0f : 0.0f;
    }
    return rates;
}

// A strand's bend straightens it by bend in each of the step's passes, against its gravity. The
// bend of a step takes the share that a free node's gravity takes, so the strand hangs in the same
// curve.
float BendForStep(float bend, float drag, s32 passes, float frames) {
    if (passes < 1) {
        return bend;
    }
    const float push = drag > SmallestRate ? frames * (1.0f - std::pow(1.0f - drag, frames)) / drag
                                           : frames * frames;
    const float count = static_cast<float>(passes);
    const float frame_bend = 1.0f - std::pow(1.0f - bend, count);
    return 1.0f - std::pow(1.0f - push * frame_bend, 1.0f / count);
}

// The step's last pass puts each node at its link's length from its parent and leaves the parent
// where it is. Repeated in short steps, that one-way pull keeps a strand swinging. This pass runs
// after the step in its place, and gives each parent most of the opposite of its child's move.
void KeepLinkLengths(u8* chain, s32 nodes) {
    const auto places = Field<float*>(chain, StrandPlaces);
    const auto speeds = Field<float*>(chain, StrandSpeeds);
    const auto rests = Field<const float*>(chain, StrandRests);
    const float shortened = Field<float>(chain, StrandShortRatio);
    std::array<std::array<float, 3>, StrandMostNodes + 1> moves{};
    for (s32 i = 1; i < nodes; i++) {
        float* const place = places + 4 * i;
        const float* const parent = places + 4 * (i - 1);
        const std::array<float, 3> link{place[0] - parent[0], place[1] - parent[1],
                                        place[2] - parent[2]};
        const float length = std::sqrt(link[0] * link[0] + link[1] * link[1] + link[2] * link[2]);
        if (length == 0.0f) {
            continue;
        }
        const float gap = rests[i - 1] - length;
        const float share = (gap > 0.0f ? gap * shortened : gap) / length;
        for (u32 k = 0; k < 3; k++) {
            moves[i][k] = link[k] * share;
            place[k] += moves[i][k];
        }
    }
    for (s32 i = 1; i < nodes; i++) {
        for (u32 k = 0; k < 3; k++) {
            speeds[4 * i + k] += moves[i][k] - LinkReaction * moves[i + 1][k];
        }
    }
}

// A 30 fps step moves a node in a straight line and puts it back at its link's length, which takes
// speed from a link turning by w radians a frame at w^3 per frame. A shorter step takes only its
// share of that, so this takes LinkTurnShare of the rest from the step's speeds, root outwards.
void SlowLinkTurns(u8* chain, s32 nodes, float frames) {
    const auto places = Field<const float*>(chain, StrandPlaces);
    const auto speeds = Field<float*>(chain, StrandSpeeds);
    const float strength = LinkTurnShare * 2.0f * (1.0f - frames) / frames;
    std::array<float, 3> parent_before{speeds[0], speeds[1], speeds[2]};
    std::array<float, 3> parent{parent_before};
    for (s32 i = 1; i < nodes; i++) {
        const float* const place = places + 4 * i;
        const float* const parent_place = places + 4 * (i - 1);
        float* const speed = speeds + 4 * i;
        const std::array<float, 3> before{speed[0], speed[1], speed[2]};
        std::array<float, 3> link;
        std::array<float, 3> relative;
        float length = 0.0f;
        float along = 0.0f;
        for (u32 k = 0; k < 3; k++) {
            link[k] = place[k] - parent_place[k];
            relative[k] = before[k] - parent_before[k];
            length += link[k] * link[k];
            along += link[k] * relative[k];
        }
        const float share = length > 0.0f ? along / length : 0.0f;
        float across = 0.0f;
        for (u32 k = 0; k < 3; k++) {
            const float part = relative[k] - share * link[k];
            across += part * part;
        }
        const float kept =
            length > 0.0f ? 1.0f / std::sqrt(1.0f + strength * across / length) : 1.0f;
        for (u32 k = 0; k < 3; k++) {
            const float radial = share * link[k];
            speed[k] = parent[k] + radial + (relative[k] - radial) * kept;
            parent[k] = speed[k];
        }
        parent_before = before;
    }
}

// Runs the strand's step with its rates, and its waves' limit, for the step's time.
void PS4_SYSV_ABI StrandStep(u8* chain) {
    const float frames = StepFrames();
    const s32 nodes = Field<s32>(chain, StrandNodes);
    if (nodes < 1 || nodes > StrandMostNodes) {
        Core::Recompiled::Call(g_strand_step, chain);
        return;
    }
    const auto drags = Field<const float*>(chain, StrandDrags);
    StrandDragRates drag_rates{};
    if (!(frames > 0.0f) || frames >= 1.0f) {
        std::copy_n(drags, nodes, drag_rates.drags.begin());
        SetField(chain, StrandDrags, drag_rates.drags.data());
        Core::Recompiled::Call(g_strand_step, chain);
        SetField(chain, StrandDrags, drags);
        return;
    }
    const auto gravities = Field<const float*>(chain, StrandGravities);
    const auto pulls = Field<const float*>(chain, StrandPulls);
    const auto bends = Field<const float*>(chain, StrandBends);
    const float limit = Field<float>(chain, StrandWaveLimit);
    const s32 two_way_passes = Field<s32>(chain, StrandPasses);
    const s32 one_way_passes = Field<s32>(chain, StrandMorePasses);
    const s32 passes = two_way_passes + one_way_passes;
    const s32 late_pulls = (Field<s32>(chain, StrandFlags) & StrandLatePull) != 0 ? passes + 1 : 0;
    const s32 wave_node = Field<s32>(chain, StrandWaveNode);
    const s32 pushed = wave_node == 0  ? (nodes + 1) >> 1
                       : wave_node > 0 ? std::min(wave_node, nodes - 1)
                                       : std::max(wave_node + nodes, 1);
    // The step does not store node 0's speed, so that slot holds the root's last move.
    const auto places = Field<const float*>(chain, StrandPlaces);
    const auto speeds = Field<float*>(chain, StrandSpeeds);
    const std::array<float, 3> root_place{places[0], places[1], places[2]};
    const u8* const root = Field<const u8*>(chain, StrandRoot);
    std::array<float, 3> air_past_root;
    for (u32 k = 0; k < 3; k++) {
        air_past_root[k] = Field<float>(root, RootAir + 4 * k) - speeds[k];
    }

    std::array<float, StrandMostNodes> step_gravities;
    std::array<float, StrandMostNodes> step_pulls;
    std::array<float, StrandMostNodes> step_bends;
    float wave_push = 1.0f;
    for (s32 i = 0; i < nodes; i++) {
        const StrandRates rates =
            RatesForStep(gravities[i], drags[i], pulls[i], late_pulls, frames);
        step_gravities[i] = rates.gravity;
        drag_rates.drags[i] = rates.drag;
        for (u32 k = 0; k < 3; k++) {
            drag_rates.offsets[k][i] = rates.air * air_past_root[k];
        }
        step_pulls[i] = rates.pull;
        step_bends[i] = BendForStep(bends[i], drags[i], passes, frames);
        if (i == pushed) {
            wave_push = rates.push;
        }
    }
    SetField(chain, StrandGravities, step_gravities.data());
    SetField(chain, StrandDrags, drag_rates.drags.data());
    SetField(chain, StrandPulls, step_pulls.data());
    SetField(chain, StrandBends, step_bends.data());
    SetField(chain, StrandWaveLimit, limit * frames);
    SetField(chain, StrandPasses, passes);
    SetField(chain, StrandMorePasses, 0);
    t_strand_wave_push = wave_push;
    Core::Recompiled::Call(g_strand_step, chain);
    SetField(chain, StrandGravities, gravities);
    SetField(chain, StrandDrags, drags);
    SetField(chain, StrandPulls, pulls);
    SetField(chain, StrandBends, bends);
    SetField(chain, StrandWaveLimit, limit);
    SetField(chain, StrandPasses, two_way_passes);
    SetField(chain, StrandMorePasses, one_way_passes);
    for (u32 k = 0; k < 3; k++) {
        speeds[k] = places[k] - root_place[k];
    }
    KeepLinkLengths(chain, nodes);
    SlowLinkTurns(chain, nodes, frames);
}

// Runs the strand's waves on its speed for a 30 fps frame, then gives their push and the advance
// of their phases the step's share.
void PS4_SYSV_ABI StrandWave(float speed, float size, u8* waves, float* push) {
    const auto function = reinterpret_cast<StrandWaveFunctionType>(
        const_cast<u8*>(Hooks::Code(g_module_base, StrandWaveFunction)));
    const float frames = StepFrames();
    if (!(frames > 0.0f) || frames >= 1.0f) {
        Core::Recompiled::Call(function, speed, size, waves, push);
        return;
    }
    const float frame_speed = speed / frames;
    Core::Recompiled::Call(function, frame_speed, size, waves, push);
    for (u32 i = 0; i < 4; i++) {
        push[i] *= t_strand_wave_push;
    }
    const s32 count = Field<s32>(waves, 0);
    const auto table = Field<u8*>(waves, WavesTable);
    for (s32 i = 0; i < count; i++) {
        u8* const place = table + i * sizeof(StrandWaveEntry);
        StrandWaveEntry wave;
        std::memcpy(&wave, place, sizeof(wave));
        float advance = wave.rate;
        if (frame_speed > wave.limit) {
            advance += frame_speed * PhaseFactor * wave.excess_rate;
        }
        if (wave.reference > 0.0f) {
            advance *= wave.reference / size;
        }
        const float phase = wave.phase + (frames - 1.0f) * advance;
        SetField(place, offsetof(StrandWaveEntry, phase), phase - std::floor(phase));
    }
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    g_module_base = module_base;
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the hair's hook");
        return false;
    }
    Xbyak::Label game_rate;
    const void* limit = c.getCurr();
    c.vmovss(xmm1, dword[rbx + 0xa8]);
    c.vmulss(xmm1, xmm1, dword[rip + code(StepTime)]);
    c.vmulss(xmm1, xmm1, dword[rip + game_rate]);
    c.vsubss(xmm1, xmm0, xmm1);
    c.jmp(code(LimitNext));
    const void* size = SizeStub(c, code(SizeNext));
    const void* push = c.getCurr();
    c.vmovss(xmm0, dword[rip + code(FloatPushAmount)]);
    c.vmulss(xmm0, xmm0, dword[rip + code(StepTime)]);
    c.vmulss(xmm0, xmm0, dword[rip + game_rate]);
    c.jmp(code(FloatPushNext));
    const void* swirl = c.getCurr();
    c.lea(rsp, ptr[rsp - RedZone - 16]);
    c.vmovups(ptr[rsp], xmm1);
    c.vbroadcastss(xmm1, dword[rip + code(StepTime)]);
    c.vmulps(xmm0, xmm0, xmm1);
    c.vbroadcastss(xmm1, dword[rip + game_rate]);
    c.vmulps(xmm0, xmm0, xmm1);
    c.vmovups(xmm1, ptr[rsp]);
    c.lea(rsp, ptr[rsp + RedZone + 16]);
    c.vaddps(xmm0, xmm0, xword[rax + 0x70]);
    c.vmovaps(xword[rax + 0x70], xmm0);
    c.jmp(code(SwirlNext));
    const auto style_push = [&](VAddr amount, VAddr next) {
        const void* stub = c.getCurr();
        c.vmovss(xmm1, dword[rip + code(StepTime)]);
        c.vmulss(xmm1, xmm1, dword[rip + game_rate]);
        c.vshufps(xmm1, xmm1, xmm1, 0);
        c.vmulps(xmm1, xmm1, xword[rip + code(amount)]);
        c.vaddps(xmm0, xmm0, xmm1);
        c.jmp(code(next));
        return stub;
    };
    const void* jupiter = style_push(JupiterPushAmount, JupiterPushNext);
    const void* lunar = style_push(LunarPushAmount, LunarPushNext);
    const void* field = c.getCurr();
    c.vbroadcastss(xmm1, dword[rip + code(StepTime)]);
    c.vmulps(xmm0, xmm0, xmm1);
    c.vbroadcastss(xmm1, dword[rip + game_rate]);
    c.vmulps(xmm0, xmm0, xmm1);
    c.vaddps(xmm0, xmm0, xword[rax + 0x70]);
    c.vmovaps(xword[rax + 0x70], xmm0);
    c.jmp(code(FieldPushNext));
    const void* style_gravity = c.getCurr();
    c.vbroadcastss(xmm0, dword[rip + code(FrameTime)]);
    c.jmp(code(StyleGravityNext));
    const void* root_gravity = c.getCurr();
    c.vmovss(xmm0, dword[rip + code(StepTime)]);
    c.vmulss(xmm0, xmm0, dword[rip + game_rate]);
    c.vshufps(xmm0, xmm0, xmm0, 0);
    c.vmulps(xmm0, xmm0, xword[r14 + 0x50]);
    c.jmp(code(RootGravityNext));
    const void* dress_gravity = c.getCurr();
    c.vmovaps(xmm0, xword[rdi + 0x50]);
    c.jmp(code(DressGravityNext));
    const void* strand_wave = c.getCurr();
    c.mov(rax, reinterpret_cast<u64>(&StrandWave));
    c.call(rax);
    c.jmp(code(StrandWaveNext));
    const void* strand_drag = c.getCurr();
    Xbyak::Label plain_drag;
    c.cmp(dword[r14 + StrandNodes], StrandMostNodes);
    c.jg(plain_drag);
    c.vmovss(xmm3, dword[rax + StrandOffsets]);
    c.vinsertps(xmm3, xmm3, dword[rax + StrandOffsets + StrandOffsetStride], 0x10);
    c.vinsertps(xmm3, xmm3, dword[rax + StrandOffsets + 2 * StrandOffsetStride], 0x20);
    c.vaddps(xmm1, xmm1, xmm3);
    c.L(plain_drag);
    c.vbroadcastss(xmm3, dword[rax]);
    c.jmp(code(StrandDragNext));
    c.L(game_rate);
    c.dd(std::bit_cast<u32>(GameRate));
    if (Hooks::Replace(module_base, Limit, c, capacity, limit) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the hair's step is not the expected code");
        return false;
    }
    if (Hooks::Replace(module_base, Size, c, capacity, size) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the hair's flutter is not the expected code");
        return false;
    }
    if (Hooks::Replace(module_base, FloatPush, c, capacity, push) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the float's push on the hair is not the expected code");
        return false;
    }
    if (Hooks::Replace(module_base, Swirl, c, capacity, swirl) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: Kat's swirl on the hair is not the expected code");
        return false;
    }
    if (Hooks::Replace(module_base, JupiterPush, c, capacity, jupiter) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, LunarPush, c, capacity, lunar) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the styles' push on the hair is not the expected code");
        return false;
    }
    if (Hooks::Replace(module_base, FieldPush, c, capacity, field) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the field's push on the hair is not the expected code");
        return false;
    }
    if (Hooks::Replace(module_base, StyleGravity, c, capacity, style_gravity) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, RootGravity, c, capacity, root_gravity) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, DressGravity, c, capacity, dress_gravity) !=
            Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the hair's gravity is not the expected code");
        return false;
    }
    const void* strand_step = nullptr;
    if (Hooks::Replace(module_base, StrandEntry, c, capacity,
                       reinterpret_cast<const void*>(&StrandStep),
                       &strand_step) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, StrandWaveCall, c, capacity, strand_wave) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, StrandDragLoad, c, capacity, strand_drag) !=
            Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the strands' step is not the expected code");
        return false;
    }
    g_strand_step = reinterpret_cast<StrandStepFunction>(const_cast<void*>(strand_step));
    return true;
}

} // namespace Gr2Port::HairFlutter
