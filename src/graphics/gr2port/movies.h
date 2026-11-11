// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The game's movies: for each new frame AvPlayer decodes, the game's FUN_01519400 runs a compute
// job on its own queue that copies the frame, NV12 in AvPlayer's padded layout, into a surface of
// its own, 1080 rows of luma and then the chroma. The job writes GPU memory only, so the hook
// copies AvPlayer's frame, which is in guest memory, when the job runs, and the renderer uploads
// that copy. FUN_01519d40 keeps a frame by a DMA from a surface into a new one, which the hook
// repeats on the copies, in the game's order.
namespace Gr2Port::Movies {

/// The surfaces whose last copy the hooks keep: each player turns over three surfaces, and a scene
/// plays several, the televisions and the screen's movie.
inline constexpr u32 Surfaces = 32;

/// Installs the hooks on the returns of the game's movie update and of its frame keeping.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The game's last copy into a surface: the copy of its frame in AvPlayer's layout and the job's
/// constants, the destination's pitch, the width and height copied, the source's pitch and height,
/// and the crop's left and top.
struct Frame {
    bool found;
    /// The slot of the surface, below Surfaces.
    u32 slot;
    /// Changes with each job into the surface; jobs and keeps share the count.
    u32 generation;
    VAddr source;
    std::array<u32, 7> constants;
};

/// Any thread: the last copy into the surface at address.
Frame Find(VAddr surface);

/// Render thread, once a frame: frees the frame copies of movie slots left unwritten, keeping
/// each published frame a surface still shows.
void Collect();

/// A slot's surface and the generation of its last job; a surface of 0 for an empty slot.
struct Job {
    VAddr surface;
    u32 generation;
};

/// Any thread: the surface of slot and its last job's generation.
Job Peek(u32 slot);

/// A surface FUN_01519d40 made to keep a movie's frame, which it fills with a DMA from the movie's
/// surface on the GPU: that surface, and the generation of the last DMA, as Frame's.
struct Kept {
    VAddr source;
    u32 generation;
    /// The frame the DMA copied, as Write takes it: the copy of the source's last job when the
    /// game kept it. Not found when no job into the source was known then.
    Frame frame;
};

/// Any thread: the last DMA into the kept surface at address; a source of 0 for none.
Kept FindKept(VAddr surface);

/// Copies frame's source into out as the game's job does, out holding rows rows of pitch bytes.
void Write(const Frame& frame, u8* out, u32 pitch, u32 rows);

} // namespace Gr2Port::Movies
