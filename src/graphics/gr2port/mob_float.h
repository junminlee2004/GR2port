// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The people Kat's gravity catches float and turn in the air. Their tasks turn a person each
// frame, on top of the turns of the frames before, by an angle made for a 30 fps frame, so the
// people spin faster as the frame rate rises:
//   FUN_0109cb20  a sway that follows the task's clock
//   FUN_0109d030  a steady turn once the person is in the field for half a second
//   FUN_00be33b0  a tilt that fades, for another kind of person
//   FUN_00c02370  a steady tumble, for a third kind
// The stubs here multiply each angle by the game step's time in 30 fps frames, which is 1 at
// 30 fps, and make the tilt fade by that time. The three tasks that lift a person also raise
// and drop the person by an amount a frame; the stubs multiply these amounts the same way.
namespace Gr2Port::MobFloat {

/// Scales the turns, the rise and the fall of the floating people by the game step's time.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::MobFloat
