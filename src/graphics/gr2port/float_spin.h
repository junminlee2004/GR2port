// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// FUN_008e1b80 turns Kat in the air on each game step by three rotations, whose angles in
// radians come from two leans that the left stick and the camera's movement steer. The frame
// rate patches multiply two of the angles by the game step's time in code caves. The third is
// the lean that the stick's forward and back steer, up to 0.05 rad a step in the float, which
// at 240 fps turns Kat 8 times as fast as at 30 fps. The stub here multiplies it by the game
// step's time in 30 fps frames, which is 1 at 30 fps.
namespace Gr2Port::FloatSpin {

/// Scales Kat's turn in the air by the game step's time.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::FloatSpin
