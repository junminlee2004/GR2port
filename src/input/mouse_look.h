// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

// The mouse turns Gravity Rush 2's cameras directly. Each mouse event adds its movement here, and
// each camera update takes what has come since the last, so a camera turns in every frame the
// mouse moves in. While no camera has taken any lately, as in menus and scenes, the mouse moves
// the right stick instead.
namespace Input::MouseLook {

/// A mouse event: adds its movement in mouse counts. While no camera takes the mouse, the
/// movement is not kept.
void Add(float dx, float dy);

/// Mouse polling: whether a camera takes the mouse.
bool Taken();

/// Camera update: the movement in mouse counts since the last call.
std::pair<float, float> Take();

} // namespace Input::MouseLook
