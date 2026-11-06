// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

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
/// quits, and waits until the main thread has done so. Returns null on failure.
SDL_Window* Create();

/// The window's size in pixels as of its last resize. Any thread.
Size PixelSize();

/// Sets the window's title without waiting.
void SetTitle(std::string title);

} // namespace Gr2Port::Window
