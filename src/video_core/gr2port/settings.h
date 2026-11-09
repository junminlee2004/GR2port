// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>

#include "common/types.h"

// GR2port's own settings: how the renderer's window shows and which built-in patches apply. They
// are the GR2port section of the user folder's config.json.
namespace Gr2Port::Settings {

enum class VSync : u32 {
    Off,
    DoubleBuffered,
    TripleBuffered,
};

struct Point {
    s32 x;
    s32 y;
};

struct Display {
    bool fullscreen{false};
    /// With fullscreen, switches the monitor to width x height at refresh_rate.
    bool exclusive{false};
    /// A point on the chosen monitor in desktop pixels, or none for the primary monitor.
    std::optional<Point> monitor;
    /// The window's size, or the display mode in exclusive fullscreen.
    u32 width{1280};
    u32 height{720};
    /// In Hz, 0 for the monitor's current rate.
    u32 refresh_rate{0};
    VSync vsync{VSync::TripleBuffered};
};

struct Game {
    /// Frames per second: 30 is the game's own rate, the others apply that frame rate's patch.
    u32 frame_rate{30};
    /// "16:9", the game's own shape, or the aspect of the patch that widens the picture.
    std::string aspect{"16:9"};
    /// The size the game renders at, a GamePatches::RenderSizes name; "1080p" is the game's own.
    std::string render_size{"1080p"};
    bool dlc_entitlements{true};
    /// The Halloween photo items mod pack. GR2Fork's setting gr2HalloweenItems holds it.
    bool halloween_items{true};
    /// Extras the player turned on or off, by patch name; the others keep the patch file's flag.
    std::map<std::string, bool> extras;
};

struct Values {
    Display display;
    Game game;
};

/// Any thread: the settings, read from config.json on first use.
Values Get();

/// Any thread: stores the settings and writes them to config.json, keeping its other sections.
bool Set(const Values& values);

} // namespace Gr2Port::Settings
