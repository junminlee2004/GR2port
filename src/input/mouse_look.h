// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

// The mouse turns Gravity Rush 2's cameras directly. Mouse polling adds the mouse's movement here,
// and each camera update takes it. While no camera has taken any lately, as in menus and scenes,
// the mouse moves the right stick instead.
namespace Input::MouseLook {

/// Mouse polling: adds a movement in mouse counts. Returns false when no camera takes the mouse,
/// and the movement is then not kept.
bool Add(float dx, float dy);

/// Camera update: the movement in mouse counts since the last call.
std::pair<float, float> Take();

} // namespace Input::MouseLook
