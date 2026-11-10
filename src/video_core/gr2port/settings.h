// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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

/// Mouse look speeds, in percent of the base speed: on foot and while shifting gravity, each
/// horizontal and vertical.
struct Mouse {
    u32 foot_horizontal{100};
    u32 foot_vertical{100};
    u32 shift_horizontal{100};
    u32 shift_vertical{100};
    /// Button prompts: "auto" shows the connected pad's, or the keyboard's when none is connected;
    /// "playstation", "xbox", "switch", "steamdeck", "steamcontroller" and "keyboard" always show
    /// those.
    std::string prompts{"auto"};
    /// The control type of the game's Controls option as the game last used it, 0 to 4 for Type A
    /// to Type E. The keyboard prompts label the buttons for it.
    u32 control_type{0};
};

/// A pass's quality: Original draws it as the game does, the lower ones cost less and show less.
enum class Quality : u32 {
    Original,
    Medium,
    Low,
    Off,
};

/// What the renderer draws and how finely. A preset sets them all; "custom" is any other mix.
struct Graphics {
    std::string preset{"ps4"};
    /// The sun's shadow maps: Original 2048, 2048 and 4096 texels a side, Medium 2048 for each,
    /// Low 1024, 1024 and 2048; Off draws no shadows.
    Quality shadows{Quality::Original};
    /// The fog's grid of cells: Original 160 x 90 x 128, Medium 120 x 68 x 96, Low 80 x 45 x 64.
    Quality fog{Quality::Original};
    bool ambient_occlusion{true};
    bool motion_blur{true};
    bool antialiasing{true};
    bool bloom{true};
    /// Texture streaming: the device memory GR2port keeps within, in MB; 0 takes the budget the
    /// driver gives the process. With less, textures keep fewer mip levels. Presets leave it.
    u32 texture_streaming_mb{0};
};

struct Preset {
    std::string_view key;
    std::string_view name;
    Quality shadows;
    Quality fog;
    bool ambient_occlusion;
    bool motion_blur;
    bool antialiasing;
    bool bloom;
    /// The aspect and render size the preset also sets, or empty to leave them.
    std::string_view aspect;
    std::string_view render_size;
};

/// From the cheapest to the game's own.
inline constexpr std::array Presets{
    Preset{"lowest", "Lowest", Quality::Off, Quality::Low, false, false, false, false, {}, {}},
    Preset{"low", "Low", Quality::Low, Quality::Low, false, false, false, true, {}, {}},
    Preset{"steamdeck", "Steam Deck", Quality::Low, Quality::Low, true, false, false, true, "16:10",
           "720p"},
    Preset{"medium", "Medium", Quality::Medium, Quality::Medium, true, true, false, true, {}, {}},
    Preset{"ps4",
           "PS4 Original",
           Quality::Original,
           Quality::Original,
           true,
           true,
           true,
           true,
           {},
           {}},
};

/// The preset's passes, with its key.
Graphics FromPreset(const Preset& preset);

/// The key of the preset that game and graphics match, or "custom".
std::string_view MatchPreset(const Game& game, const Graphics& graphics);

struct Values {
    Display display;
    Game game;
    Mouse mouse;
    Graphics graphics;
};

/// Any thread: the settings, read from config.json on first use.
Values Get();

/// Any thread: stores the settings and writes them to config.json, keeping its other sections.
bool Set(const Values& values);

} // namespace Gr2Port::Settings
