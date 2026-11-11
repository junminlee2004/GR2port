// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct SDL_Window;

// The renderer's window. SDL creates and changes windows on the main thread only, so these calls
// queue their work there.
namespace Gr2Port::Window {

struct Size {
    u32 width;
    u32 height;
};

/// Creates the window, a child of the game's window so that closing the game's window still
/// quits, and waits until the main thread has done so. On Wayland the window has no parent.
/// Returns null on failure.
SDL_Window* Create();

/// The window's size in pixels as of its last resize. Any thread.
Size PixelSize();

/// The SDL ids of the window and of the game's window, or 0 before Create. Any thread.
u32 Id();
u32 GameId();

/// Main thread: switches the window between windowed and fullscreen as the settings choose it, on
/// the monitor it is on. Returns false before the window exists.
bool ToggleFullscreen();

} // namespace Gr2Port::Window
