// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Native mouse look. The game's FUN_0155be10 turns a camera's orientation by the look input of the
// right stick, with a dead zone and a speed that ramps up; three camera updates call it, the
// gravity shift camera's among them. Its replacement runs it, then turns the orientation by the
// mouse's movement since the last call: yaw about the camera frame's up axis, pitch about the
// camera's own right axis, scaled by the game's camera sensitivity and inverted with its options.
// The camera on foot, FUN_01567bd0, turns by the stick itself; the mouse turns its orientation the
// same way as it starts.
namespace Gr2Port::MouseCamera {

/// Replaces the game's camera turn with the one that adds the mouse.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::MouseCamera
