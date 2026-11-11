// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Event timelines at any frame rate. A timeline node (FUN_010f7320) counts whole frames of 1/30 s
// and samples its tracks, the curves and animation clips of its actors and cameras, only when the
// count moves. The frame rate patches keep the part of a frame that has passed in a global; here
// a playing node samples its tracks every game frame at the count plus that part, and so do the
// demo actors that place themselves from their tracks (FUN_010f18e0). A node's camera applied
// before its first sample, which still holds the camera it was created with, is replaced by the
// camera applied last.
namespace Gr2Port::Timeline {

/// Hooks the timeline player and the camera's application; the sampling between frames needs a
/// frame rate patch.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::Timeline
