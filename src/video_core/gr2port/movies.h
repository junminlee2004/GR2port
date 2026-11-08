// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
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
// its own, 1080 rows of luma and then the chroma. The job writes GPU memory only, so the renderer
// does the same copy from AvPlayer's frame, which is in guest memory.
namespace Gr2Port::Movies {

/// Installs the hook on the return of the game's movie update.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The game's last copy into a surface: its source and the job's constants, the destination's
/// pitch, the width and height copied, the source's pitch and height, and the crop's left and top.
struct Frame {
    bool found;
    /// Counts the jobs into the surface.
    u32 generation;
    /// Game frames since the job.
    u64 age;
    VAddr source;
    std::array<u32, 7> constants;
};

/// Any thread: the last copy into the surface at address.
Frame Find(VAddr surface);

/// Copies frame's source into out as the game's job does, out holding rows rows of pitch bytes.
void Write(const Frame& frame, u8* out, u32 pitch, u32 rows);

} // namespace Gr2Port::Movies
