// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The slots the game's draw thread writes each frame's scene into and the render thread draws
// from. The GPU reads a slot in place. The draw thread never waits: when no slot is free, its
// frame is skipped.
namespace Gr2Port::SceneRing {

inline constexpr u32 Slots = 4;
inline constexpr u64 SlotBytes = 24_MB;

/// Hands the ring's memory to the draw thread: base holds Slots slots of SlotBytes, mapped at
/// cpu and at address on the GPU.
void Attach(u8* cpu, u64 address);

/// Draw thread: a free slot to write this frame's scene into, or null.
u8* Begin();

/// Draw thread: the GPU address of data in the slot Begin returned.
u64 Address(const u8* data);

/// Draw thread: the slot Begin returned holds frame's scene.
void Publish(u64 frame);

struct Packet {
    u32 slot;
    u64 frame;
    const u8* data;
    u64 address;
};

/// Render thread: waits for a frame newer than seen, then takes the newest published one and
/// frees the older ones. False when none was left to take.
bool Take(u64 seen, Packet& packet);

/// Render thread: the GPU no longer reads slot.
void Release(u32 slot);

/// Frames the draw thread skipped because no slot was free.
u64 Skipped();

} // namespace Gr2Port::SceneRing
