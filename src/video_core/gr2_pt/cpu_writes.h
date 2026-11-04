// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/types.h"

// The CPU writes that reach guest memory the renderer reads through the buffer cache. The buffer
// cache reports each range the CPU writes after its pages were uploaded, and each range it drops;
// the renderer watches the blocks it reads and takes the reports that touch them.
namespace Gr2Pt::CpuWrites {

inline constexpr u32 BlockBits = 16;

struct Range {
    VAddr begin;
    VAddr end;
};

/// Reports the writes to the blocks of [address, address + size) from now on. A range is watched
/// before its pages are uploaded, so that the first write after the upload is reported.
void Watch(VAddr address, u64 size);

/// Moves the writes reported since the last call into writes.
void Take(std::vector<Range>& writes);

} // namespace Gr2Pt::CpuWrites
