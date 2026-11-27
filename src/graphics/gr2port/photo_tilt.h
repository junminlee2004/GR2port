// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Photo mode's camera rolls and tilts by the right stick while the input of Rotate Camera, L3 on a
// pad, is held, and by the mouse while its key is held. Right and left roll it clockwise and
// counterclockwise about its line of sight. Up tilts it toward the ground and down toward the sky,
// as far as straight down and straight up. The camera manager's view takes the turn each frame,
// after the game's cameras made it, so the game's own camera keeps its place; putting the camera
// away ends the turn.
namespace Gr2Port::PhotoTilt {

/// Hooks the camera manager's view.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Input handling, at each update of the pad: the right stick's tilt while the camera is out and
/// Rotate Camera's input is held, x to the right and y downwards, from -1 to 1; 0 and 0 otherwise.
void SetStick(float x, float y);

/// Input handling: whether Rotate Camera's key is held.
void SetKey(bool held);

/// Any thread: whether the mouse turns the photo mode's camera in place of the game's cameras.
bool TakesMouse();

} // namespace Gr2Port::PhotoTilt
