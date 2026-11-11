// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

struct SDL_Window;

// The game's window, the only one: the emulator takes its input from it and the renderer draws in
// it. SDL creates and changes windows on the main thread only.
namespace Gr2Port::Window {

struct Size {
    u32 width;
    u32 height;
};

/// Main thread, at the emulator's start: creates the window, hidden. Returns null on failure.
SDL_Window* Open();

/// Any thread but the main one: shows the window as the settings choose it, waits until the main
/// thread has done so, and returns it. Returns null without a window.
SDL_Window* Show();

/// The window's size in pixels as of its last resize. Any thread.
Size PixelSize();

/// The window's SDL id, or 0 before Open. Any thread.
u32 Id();

/// Main thread: switches the window between windowed and fullscreen as the settings choose it, on
/// the monitor it is on.
void ToggleFullscreen();

} // namespace Gr2Port::Window
