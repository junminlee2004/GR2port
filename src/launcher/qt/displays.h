// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>
#include <SDL3/SDL_video.h>

#include "common/types.h"

// The monitors and their display modes as SDL lists them, which is how the game's window finds
// them. SDL's video subsystem must be started.
namespace Gr2Port::Launcher {

struct Monitor {
    SDL_DisplayID id;
    /// Its place in SDL's list, from 1, and its name.
    std::string label;
    SDL_Rect area;
    bool primary;
};

struct Size {
    u32 width;
    u32 height;
    std::vector<u32> rates;
};

struct Modes {
    std::vector<Size> sizes;
    u32 current_width = 0;
    u32 current_height = 0;
    u32 current_rate = 0;
};

std::vector<Monitor> ListMonitors();

/// The monitor's sizes of at least 800 x 600, each with its refresh rates in whole Hz.
Modes ListModes(SDL_DisplayID monitor);

} // namespace Gr2Port::Launcher
