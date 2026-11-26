// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/part_hits.h"

namespace Gr2Port::PartHits {

namespace {

// FUN_00bb6080, which gives an event to a body part: (part, event).
constexpr Hooks::Entry PartEvent{0xbb6080, 6, Hooks::FramePrologue};

// The game's count of frames, which it zeroes when a level loads.
constexpr VAddr GameFrames = 0x1f302e4;

// A byte in the part. Zero makes the part ignore hits for 0.1 s after one lands.
constexpr u64 NoHitPause = 0x5a0;

// An event holds its type at +8. A hit holds its attacker at +0x10, and the part writes the result
// that the attack reads to +0x68.
constexpr u64 EventTypeField = 8;
constexpr u64 HitAttacker = 0x10;
constexpr u64 HitResult = 0x68;

// The id of the type of a hit, DAT_01ee0828, which FUN_010204d0 sets.
constexpr u32 HitType = 0x421f6479;

// The frame rate patches' length of this game frame in 30 fps frames, at most 1.
constexpr VAddr FrameShare = 0x1f7c600;

struct EventType {
    u32 id;
    const EventType* parent;
};

// One attack in contact with one part. An attack is its attacker and the game function that
// delivers its hits, because one attacker can use several attacks at once.
struct Contact {
    VAddr part;
    VAddr attacker;
    VAddr caller;
    u64 result;
    u32 last_frame;
    u32 landed_frame;
    // The time since the last hit that landed, in 30 fps frames.
    float waited;
};

using PartEventFunction = void PS4_SYSV_ABI (*)(VAddr part, VAddr event);

PartEventFunction g_part_event = nullptr;
const u32* g_game_frames = nullptr;
const float* g_frame_share = nullptr;
std::array<Contact, 32> g_contacts{};

bool IsHit(VAddr event) {
    const auto* type = *reinterpret_cast<const EventType* const*>(event + EventTypeField);
    for (; type != nullptr; type = type->parent) {
        if (type->id == HitType) {
            return true;
        }
    }
    return false;
}

// Whether the contact's hit on this frame lands.
bool Lands(Contact& contact, u32 frame) {
    const u32 since = frame - contact.last_frame;
    contact.last_frame = frame;
    if (since == 0) {
        return contact.landed_frame == frame;
    }
    if (since == 1) {
        contact.waited += *g_frame_share;
        if (contact.waited < 1.0f) {
            return false;
        }
        contact.waited -= 1.0f;
    } else {
        contact.waited = 0.0f;
    }
    contact.landed_frame = frame;
    return true;
}

void PS4_SYSV_ABI OnPartEvent(VAddr part, VAddr event) {
    const VAddr caller = Core::Recompiled::GuestCaller(__builtin_return_address(0));
    if (*reinterpret_cast<const u8*>(part + NoHitPause) == 0 || !IsHit(event)) {
        Core::Recompiled::Call(g_part_event, part, event);
        return;
    }
    const VAddr attacker = *reinterpret_cast<const VAddr*>(event + HitAttacker);
    const u32 frame = *g_game_frames;
    Contact* contact = nullptr;
    Contact* idlest = g_contacts.data();
    for (Contact& entry : g_contacts) {
        if (entry.part == part && entry.attacker == attacker && entry.caller == caller) {
            contact = &entry;
            break;
        }
        if (frame - entry.last_frame > frame - idlest->last_frame) {
            idlest = &entry;
        }
    }
    bool lands = true;
    if (contact != nullptr) {
        lands = Lands(*contact, frame);
    } else {
        contact = idlest;
        *contact = {part, attacker, caller, 0, frame, frame, 0.0f};
    }
    if (!lands) {
        *reinterpret_cast<u64*>(event + HitResult) = contact->result;
        return;
    }
    Core::Recompiled::Call(g_part_event, part, event);
    contact->result = *reinterpret_cast<const u64*>(event + HitResult);
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    const void* original = nullptr;
    if (!Hooks::Hooked(Hooks::Replace(module_base, PartEvent, c, capacity,
                                      reinterpret_cast<const void*>(&OnPartEvent), &original))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the body part's hit is not the expected code");
        return false;
    }
    g_part_event = reinterpret_cast<PartEventFunction>(const_cast<void*>(original));
    g_game_frames = reinterpret_cast<const u32*>(Hooks::Code(module_base, GameFrames));
    g_frame_share = reinterpret_cast<const float*>(Hooks::Code(module_base, FrameShare));
    return true;
}

} // namespace Gr2Port::PartHits
