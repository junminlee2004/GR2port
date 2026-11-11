// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/types.h"

// The game still fills command buffers and submits them, and nothing draws from them. Each submit
// is walked once, on the thread that submits it, for what the game's CPU code waits on or reads
// back: the labels and data its packets write to memory, occlusion counters, semaphores, a compute
// ring's read pointer, the end-of-pipe and release interrupts, and the flip. A packet that waits
// for the GPU does not wait. The caller holds the submit lock.
namespace Gr2Port::Submits {

/// The compute pipes a game maps queues on, and the queues of each. The system keeps an eighth
/// pipe.
constexpr u32 NumComputePipes = 7;
constexpr u32 NumQueuesPerPipe = 8;
constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;

/// A compute ring the game mapped.
struct Ring {
    VAddr base;
    u32* read_pointer;
    u32 size_dwords;
    u32 pipe;
    /// The start of a packet that the end of a submit cut, kept for the next submit.
    std::vector<u32> held;
};

/// A draw command buffer.
void Graphics(std::span<const u32> dcb);

/// The packets the game added to a compute ring.
void Compute(Ring& ring, std::span<const u32> acb);

} // namespace Gr2Port::Submits
