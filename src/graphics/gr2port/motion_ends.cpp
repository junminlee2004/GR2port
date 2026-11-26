// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <cstring>
#include <mutex>
#include <vector>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/motion_ends.h"

namespace Gr2Port::MotionEnds {

namespace {

// FUN_008f6430, for each motion of a layer: mov edx, [rcx + 0x3c] loads how many more times the
// motion plays, 0 for once, and add eax, r15d adds the layer's step in ticks to the playhead in
// eax. esi holds the motion's length in ticks, at least 1. The game writes r13d, xmm5 and xmm7
// before it reads them again.
constexpr Hooks::Entry Advance{0x8fb830, 6, {0x8b, 0x51, 0x3c, 0x44, 0x01, 0xf8}};

constexpr float Half = 0.5f;
constexpr float One = 1.0f;

// The stub and its constants.
constexpr size_t StubBytes = 128;

// bool FUN_013cdf30(float blend, player, u32 layer, u32 motion, s32 repeats, u32 flags): plays a
// motion on a layer. The motion fades in over blend seconds of motion time while the layer's last
// motion fades out over the same time, and a blend of 0 replaces that motion at once.
constexpr Hooks::Entry Play{0x13cdf30, 6, Hooks::FramePrologue};

// The puppet the player controls. Its motion controller at + 0x478 plays motions at + 0x70.
constexpr VAddr Puppet = 0x1f30190;
constexpr u64 PuppetMotions = 0x478;
constexpr u64 MotionsPlayer = 0x70;

// From the player: the rate of motion time, and the vector of layers (begin + 8, end + 0x10).
constexpr u64 PlayerRate = 0x40;
constexpr u64 PlayerLayers = 0x38;

// A layer: its rate, and its list of motions, whose node at + 0x28 is the last one or the list's
// head at + 8. A node points to its motion at + 0x18.
constexpr u64 LayerRate = 0x4c;
constexpr u64 LayerHead = 0x08;
constexpr u64 LayerLast = 0x28;
constexpr u64 NodeMotion = 0x18;

// A motion: its length and playhead in ticks, and how many more times it plays and has played,
// fewer than 0 for always.
constexpr u64 MotionLength = 0x38;
constexpr u64 MotionRepeats = 0x3c;
constexpr u64 MotionPlayhead = 0x40;
constexpr u64 MotionPlayed = 0x44;

constexpr float FrameRate = 30.0f;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// The frame rate patches' length of this game frame in 30 fps frames, at most 1. The step's time
// over this is a 30 fps frame at the current time scale.
constexpr VAddr FrameShare = 0x1f7c600;

// FUN_00978380(float time, events, u32 motion, ..., callback) runs a character's events once a
// frame for its newest motion, the last one of its puppet's first layer, with that motion's
// playhead in seconds: an event fires on the first frame where its start is at most time +
// EventSlack. When the motion is another one than at the last call, or its playhead went back,
// the call first runs the callback, which for the player's characters is FUN_0090d250(character,
// u32 motion): it ends the effects of the last motion's events and stops their sounds, whose
// handles it keeps from + 0x5c20 to + 0x5c28. A character's puppet is at + 0x210, its events at
// + 0x4fb0.
constexpr Hooks::Entry EventUpdate{0x978380, 6, Hooks::FramePrologue};
constexpr Hooks::Entry EventsEnded{0x90d250, 6, Hooks::FramePrologue};
// FUN_0090d380(character), which the game calls on its own, has the same body as FUN_0090d250.
constexpr Hooks::Entry EventsCleared{0x90d380, 6, Hooks::FramePrologue};
constexpr u64 CharacterPuppet = 0x210;
constexpr u64 CharacterEvents = 0x4fb0;
constexpr u64 CharacterSounds = 0x5c20;
constexpr u64 CharacterSoundsEnd = 0x5c28;
constexpr float TickSeconds = 0.00033333333f;
constexpr float EventSlack = 0.000333f;

// void FUN_014e1580(float fade, s32 sound): stops a sound, or releases it when it has a release.
constexpr VAddr StopSound = 0x14e1580;

using PlayFunction = u64 PS4_SYSV_ABI (*)(float blend, u64 player, u64 layer, u64 motion,
                                          u64 repeats, u64 flags);
using EventsEndedFunction = void PS4_SYSV_ABI (*)(u64 character, u64 motion);
using StopFunction = void PS4_SYSV_ABI (*)(float fade, s32 sound);

VAddr g_module_base = 0;
PlayFunction g_play = nullptr;
EventsEndedFunction g_events_ended = nullptr;
StopFunction g_stop = nullptr;

// The motion that last replaced an ended one at once on the first layer of Kat's motion player.
std::atomic<u64> g_replacing{0};

// The sounds of Kat's replaced motion, held until her events, g_held_events or 0 when none are
// held, find the new one g_held_until seconds in.
std::mutex g_held_mutex;
std::vector<s32> g_held;
std::atomic<u64> g_held_events{0};
std::atomic<float> g_held_until{0.0f};

template <typename T>
T Load(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

template <typename T>
void Store(u64 address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

// The layer at index of Kat's motion player, or 0 when player is another one or has no such
// layer.
u64 KatLayer(u64 player, u32 index) {
    const u64 puppet = Load<u64>(g_module_base + Puppet - Hooks::CodeBase);
    if (puppet == 0 || Load<u64>(puppet + PuppetMotions) + MotionsPlayer != player) {
        return 0;
    }
    const u64 layers = Load<u64>(player + PlayerLayers);
    const u64 begin = Load<u64>(layers + 8);
    if (index >= (Load<u64>(layers + 0x10) - begin) / sizeof(u64)) {
        return 0;
    }
    return Load<u64>(begin + index * sizeof(u64));
}

// The layer's last motion, or 0 when it has none.
u64 LastMotion(u64 layer) {
    const u64 node = Load<u64>(layer + LayerLast);
    if (node == 0 || node == layer + LayerHead) {
        return 0;
    }
    return Load<u64>(node + NodeMotion);
}

// Whether the layer's last motion has played to its end and holds its last pose.
bool LastMotionEnded(u64 layer) {
    const u64 motion = LastMotion(layer);
    if (motion == 0) {
        return false;
    }
    const s32 repeats = Load<s32>(motion + MotionRepeats);
    return repeats >= 0 && Load<s32>(motion + MotionPlayed) >= repeats &&
           Load<s32>(motion + MotionPlayhead) >= Load<s32>(motion + MotionLength);
}

// Stops the held sounds when events are the events that hold them.
void StopHeld(u64 events) {
    std::vector<s32> sounds;
    {
        std::scoped_lock lock{g_held_mutex};
        if (events != g_held_events) {
            return;
        }
        sounds.swap(g_held);
        g_held_events = 0;
    }
    for (const s32 sound : sounds) {
        Core::Recompiled::Call(g_stop, 0.0f, sound);
    }
}

// Before each event update: the one that finds Kat's new motion far enough in stops the held
// sounds before her events fire.
void PS4_SYSV_ABI OnEventUpdate(u32, const Hooks::Registers* registers, u64) {
    if (registers->rdi != g_held_events.load(std::memory_order_relaxed)) {
        return;
    }
    float time;
    std::memcpy(&time, &registers->xmm0, sizeof(time));
    if (time + EventSlack >= g_held_until.load(std::memory_order_relaxed)) {
        StopHeld(registers->rdi);
    }
}

// A character's events found another motion. When it is Kat's newest motion, which replaced an
// ended one at once and is less than one 30 fps frame of motion time in at the current time
// scale, the sounds of the replaced motion's events are held until her events find the new one
// that far in, where 30 fps stops them. Their effects still end now.
void PS4_SYSV_ABI OnEventsEnded(u64 character, u64 motion) {
    const u64 events = Load<u64>(character + CharacterEvents);
    StopHeld(events);
    const u64 player =
        Load<u64>(Load<u64>(character + CharacterPuppet) + PuppetMotions) + MotionsPlayer;
    const u64 layer = KatLayer(player, 0);
    if (layer != 0) {
        const u64 replacing = g_replacing.exchange(0);
        const float frame = Load<float>(player + PlayerRate) * Load<float>(layer + LayerRate) *
                            Load<float>(g_module_base + StepTime - Hooks::CodeBase) /
                            Load<float>(g_module_base + FrameShare - Hooks::CodeBase);
        const u64 begin = Load<u64>(character + CharacterSounds);
        const u64 end = Load<u64>(character + CharacterSoundsEnd);
        if (replacing != 0 && LastMotion(layer) == replacing && end > begin &&
            static_cast<float>(Load<s32>(replacing + MotionPlayhead)) * TickSeconds + EventSlack <
                frame) {
            std::scoped_lock lock{g_held_mutex};
            for (u64 at = begin; at < end; at += sizeof(s32)) {
                g_held.push_back(Load<s32>(at));
            }
            g_held_until = frame;
            g_held_events = events;
            Store<u64>(character + CharacterSoundsEnd, begin);
        }
    }
    Core::Recompiled::Call(g_events_ended, character, motion);
}

// The game clears a character's events on its own: the held sounds stop with the others.
void PS4_SYSV_ABI OnEventsCleared(u32, const Hooks::Registers* registers, u64) {
    if (g_held_events.load(std::memory_order_relaxed) != 0) {
        StopHeld(Load<u64>(registers->rdi + CharacterEvents));
    }
}

// Most bones of Kat's strides hold the first pose until tick 100, where 30 fps first shows the
// stride, and that pose is away from where the last stride ends: above 30 fps they jump to it on
// one frame and stand still until tick 100. A motion of Kat's that replaces an ended one at once
// fades in from its last pose over one 30 fps frame of motion time instead, which moves them
// steadily to the pose 30 fps shows next. The ended motion adds nothing to Kat's travel.
u64 PS4_SYSV_ABI OnPlay(float blend, u64 player, u64 layer, u64 motion, u64 repeats, u64 flags) {
    u64 replaced = 0;
    if (blend == 0.0f) {
        const u64 kat_layer = KatLayer(player, static_cast<u32>(layer));
        if (kat_layer != 0 && LastMotionEnded(kat_layer)) {
            blend =
                Load<float>(player + PlayerRate) * Load<float>(kat_layer + LayerRate) / FrameRate;
            // Her events follow her first layer.
            if (layer == 0) {
                replaced = kat_layer;
            }
        }
    }
    const u64 played = Core::Recompiled::Call(g_play, blend, player, layer, motion, repeats, flags);
    if (replaced != 0) {
        g_replacing = LastMotion(replaced);
    }
    return played;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the hook of the motions' ends");
        return false;
    }
    Xbyak::Label game_step, half, one;
    const void* advance = c.getCurr();
    const u8* next = code(Advance.address + Advance.size);
    c.mov(edx, dword[rcx + 0x3c]);
    c.test(edx, edx);
    c.jne(game_step);
    c.test(r15d, r15d);
    c.jle(game_step);
    c.mov(r13d, esi);
    c.sub(r13d, eax);
    c.jle(game_step);
    // The steps left at about the game step, at least 1, and the rest of the motion split into
    // that many.
    c.vcvtsi2ss(xmm5, xmm5, r13d);
    c.vcvtsi2ss(xmm7, xmm7, r15d);
    c.vdivss(xmm7, xmm5, xmm7);
    c.vaddss(xmm7, xmm7, dword[rip + half]);
    c.vroundss(xmm7, xmm7, xmm7, 1);
    c.vmaxss(xmm7, xmm7, dword[rip + one]);
    c.vdivss(xmm5, xmm5, xmm7);
    c.vaddss(xmm5, xmm5, dword[rip + half]);
    c.vcvttss2si(r13d, xmm5);
    c.add(eax, r13d);
    c.jmp(next);
    c.L(game_step);
    c.add(eax, r15d);
    c.jmp(next);
    c.L(half);
    c.dd(std::bit_cast<u32>(Half));
    c.L(one);
    c.dd(std::bit_cast<u32>(One));
    if (Hooks::Replace(module_base, Advance, c, capacity, advance) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the motions' advance is not the expected code");
        return false;
    }
    g_module_base = module_base;
    const void* original = nullptr;
    if (Hooks::Replace(module_base, Play, c, capacity, reinterpret_cast<const void*>(&OnPlay),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the motions' play is not the expected code");
        return false;
    }
    g_play = reinterpret_cast<PlayFunction>(const_cast<void*>(original));
    // The observers do nothing until OnEventsEnded holds sounds, so it goes in last.
    g_stop = reinterpret_cast<StopFunction>(module_base + StopSound - Hooks::CodeBase);
    const void* events_ended = nullptr;
    if (!Hooks::Hooked(
            Hooks::ObserveEntry(module_base, EventUpdate, c, capacity, &OnEventUpdate, 0)) ||
        !Hooks::Hooked(
            Hooks::ObserveEntry(module_base, EventsCleared, c, capacity, &OnEventsCleared, 0)) ||
        Hooks::Replace(module_base, EventsEnded, c, capacity,
                       reinterpret_cast<const void*>(&OnEventsEnded),
                       &events_ended) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the motions' events are not the expected code");
        return false;
    }
    g_events_ended = reinterpret_cast<EventsEndedFunction>(const_cast<void*>(events_ended));
    return true;
}

} // namespace Gr2Port::MotionEnds
