// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"
#include "graphics/gr2port/graphics_menu.h"

namespace Xbyak {
class CodeGenerator;
}

// Gravity Rush 2's photo mode draws its dark border, the viewfinder's grid and its control lists
// in the screen drawing over the view through Kat's camera. Hide UI, L3 + R2 on a pad, hides the
// screen drawing over the picture while the camera is out, but for the gesture window, and shows
// it again. Putting the camera away shows it again too. The left control list shows Hide UI and
// its buttons under its Filter Effect row, in a row of the game's own, and under that the buttons
// of Rotate Camera, PhotoTilt's roll and tilt of the camera.
namespace Gr2Port::PhotoUi {

/// The start of the keys of the added rows' texts.
inline constexpr char KeyPrefix[] = "gr2port_photo_";

/// At the executable's load.
void Install(VAddr module_base);

/// Adds the Hide UI and Rotate Camera rows to the control list. ExitRows' text hook calls
/// FindText.
bool AddRow(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Sends every draw item of the draw thread's walk through UiItems::RunItem, which reads it for the
/// screen drawing and runs it; while the screen drawing is hidden, the gesture window's items keep
/// their draws.
bool RouteItems(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The record of a key that starts with KeyPrefix, made from a record that find gives, or 0.
u64 FindText(u64 texts, const char* key, GraphicsMenu::FindTextFunction find);

/// Any thread: whether the camera is out.
bool CameraOut();

/// The filters of photo mode.
inline constexpr u32 Filters = 5;

/// Notes the camera each frame of photo mode, for Filter.
bool WatchCamera(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Any thread: the filter the player picked while the camera is out, else Filters.
u32 Filter();

/// Input handling, at a press of Hide UI: while the camera is out, hides the screen drawing over
/// the picture or shows it again.
void ToggleHidden();

/// After each of the game's pad reads: shows the screen drawing again once the camera is away.
void OnPadRead();

/// Any thread: whether the screen drawing over the picture is hidden.
bool Hidden();

/// On the draw thread, once it has read the frame of a context: forgets which of the draw items
/// of the pool with the context's number are the gesture window's, as the pool's memory is used
/// again.
void ForgetItems(u32 context);

} // namespace Gr2Port::PhotoUi
