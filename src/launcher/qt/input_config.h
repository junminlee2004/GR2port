// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The launcher's lines of the input config: the stick deadzones, the touch pad swipe switch, the
// keys of the game's functions and the pad buttons of the PlayStation buttons they use.
namespace Gr2Port::Launcher {

inline constexpr int DeadzoneLow = 1;
inline constexpr int DeadzoneHigh = 127;

/// The input config's names of the sticks, in the order of Deadzones.
inline constexpr std::array<std::string_view, 2> StickNames{"leftjoystick", "rightjoystick"};

/// The minimum and maximum deadzone of each stick.
using Deadzones = std::array<std::array<int, 2>, StickNames.size()>;

/// The "analog_deadzone = stick, min, max" lines. A stick without one keeps the parser's 1 and
/// 127.
Deadzones ReadDeadzones(const std::filesystem::path& file);

/// A line of the input config to write: the start of the line it replaces, without its comment
/// and spaces, and the line.
struct ConfigLine {
    std::string prefix;
    std::string line;
};

std::vector<ConfigLine> DeadzoneLines(const Deadzones& deadzones);

ConfigLine TouchSwipeLine(bool on);

/// Whether the input config turns touch pad swipes on: its last touchpad_swipe_enabled line.
bool ReadTouchSwipe(const std::filesystem::path& file);

/// Rewrites the input config's lines that start with each prefix and adds the missing ones at its
/// end.
bool SaveConfigLines(const std::filesystem::path& file, const std::vector<ConfigLine>& changes);

/// The keys of the game's functions (Input::GameFunctions), one input config key name per
/// function in its order, empty for none.
using KeyBindings = std::vector<std::string>;

/// GR2port's default keys.
KeyBindings DefaultKeyBindings();

/// The keys of the input config's gr2_ lines; the default keys when it has none.
KeyBindings ReadKeyBindings(const std::filesystem::path& file);

/// Writes the keys as the input config's gr2_ lines, in place of its keyboard and mouse lines for
/// the pad inputs the functions use.
bool SaveKeyBindings(const std::filesystem::path& file, const KeyBindings& keys);

/// The pad button that presses each PlayStation button the game's functions use, by the input
/// config's names: "cross" -> "r1" means R1 presses cross. A button left out has none.
using PadBindings = std::map<std::string, std::string, std::less<>>;

/// Each button presses itself, and Back presses the touch pad.
PadBindings DefaultPadBindings();

/// The input config's controller lines for those buttons; the defaults when it has none.
PadBindings ReadPadBindings(const std::filesystem::path& file);

/// Writes the buttons as the input config's controller lines for them, in place of the old ones.
bool SavePadBindings(const std::filesystem::path& file, const PadBindings& buttons);

} // namespace Gr2Port::Launcher
