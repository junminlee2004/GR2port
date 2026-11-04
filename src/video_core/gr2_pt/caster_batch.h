// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"
#include "video_core/amdgpu/resource.h"

namespace Xbyak {
class CodeGenerator;
}

// The opaque shadow casters as records for the renderer. The game's draw thread runs the item
// callback of each pass's first drawn caster, which opens the pass on the shadow map, and turns
// the other casters into records; a marker at the end of the submission hands them over.
namespace Gr2Pt::CasterBatch {

enum RecordFlags : u8 {
    CullBack = 1,
    Index32 = 2,
};

/// A caster's draw, read on the draw thread: its vertex streams, index range and matrices.
struct Record {
    AmdGpu::Buffer positions;
    // Skinned casters only.
    AmdGpu::Buffer weights;
    AmdGpu::Buffer bone_indices;
    VAddr indices;
    VAddr world;
    VAddr palette;
    VAddr view_projection;
    u32 index_bytes;
    u32 first;
    u32 count;
    // The palette's bones, 0 for a static caster.
    u16 bones;
    u8 flags;
};

/// Records drawn and records the draw thread handed to the game's callback, since the last drain.
struct Counters {
    u32 batched;
    u32 unsupported;
    u32 ring_full;
};

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The draw thread enters and leaves a cascade's caster submission; End writes its batch marker.
void Begin(u32 cascade, u32 frame);
void End();

/// The command processor's thread: lets the draw thread batch casters once the renderer is ready,
/// or returns every caster to the game's callback from the next submission on.
void Enable();
void Disable();

/// The command processor's thread: a batch's record, and the end of the records it has drawn.
const Record& Get(u32 index);
void Release(u32 end);

Counters Drain();

} // namespace Gr2Pt::CasterBatch
