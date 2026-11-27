// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// A build with CATCHER, made for the GPU Crash Catcher, counts each time a loop stops at its limit
// in the counts whose address ends its push constants; catcher_counts.cpp logs them. Other builds
// hold none of it.
#ifdef CATCHER
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(buffer_reference, std430, buffer_reference_align = 4) buffer CatcherCounts {
    uint counts[];
};

// The last push constant, at catcher_counts.h's Offset.
#define CATCHER_PUSH layout(offset = 120) CatcherCounts catcher_counts;
#define CatcherCountIf(reached, id)                                                                \
    if (reached) {                                                                                 \
        CountLimit(catcher_counts, id, 1u);                                                        \
    }
// The lanes that reach it, in one add a wave; the program has the subgroup ballot extension.
#define CatcherCountLanes(id)                                                                      \
    {                                                                                              \
        const uint catcher_lanes = subgroupBallotBitCount(subgroupBallot(true));                   \
        if (subgroupElect()) {                                                                     \
            CountLimit(catcher_counts, id, catcher_lanes);                                         \
        }                                                                                          \
    }

// The counts, in the order of catcher_counts.cpp's names.
const uint CountCharacterTurns = 0u;
const uint CountCharacterDepthTurns = 1u;
const uint CountWorldTurns = 2u;
const uint CountSpawnRing = 3u;
const uint CountSpawnParentRing = 4u;
const uint CountSpawnParents = 5u;
const uint CountSpawnPlace = 6u;

void CountLimit(CatcherCounts counts, uint id, uint amount) {
    // 0 until the catcher's buffer exists.
    if (uint64_t(counts) != 0ul) {
        atomicAdd(counts.counts[id], amount);
    }
}
#else
#define CATCHER_PUSH
#define CatcherCountIf(reached, id)
#define CatcherCountLanes(id)
#endif
