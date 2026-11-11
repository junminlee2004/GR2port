// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "common/types.h"

// The keys of the game's functions (Input::GameFunctions), one input config key name per function
// in its order, empty for none.
namespace Gr2Port::Launcher {

using KeyBindings = std::vector<std::string>;

/// GR2port's default keys.
KeyBindings DefaultKeyBindings();

/// The keys of the input config's gr2_ lines; the default keys when it has none.
KeyBindings ReadKeyBindings(const std::filesystem::path& file);

/// Writes the keys as the input config's gr2_ lines, in place of its keyboard and mouse lines for
/// the pad inputs the functions use.
bool SaveKeyBindings(const std::filesystem::path& file, const KeyBindings& keys);

/// Shows the Key Bindings dialog over owner, opened on the control type the game used last.
/// Returns the edited keys on OK.
std::optional<KeyBindings> EditKeyBindings(HWND owner, const KeyBindings& keys, u32 control_type);

/// The pad button that presses each PlayStation button the game's functions use, by the input
/// config's names: "cross" -> "r1" means R1 presses cross. A button left out has none.
using PadBindings = std::map<std::string, std::string, std::less<>>;

/// Each button presses itself, and Back presses the touch pad.
PadBindings DefaultPadBindings();

/// The input config's controller lines for those buttons; the defaults when it has none.
PadBindings ReadPadBindings(const std::filesystem::path& file);

/// Writes the buttons as the input config's controller lines for them, in place of the old ones.
bool SavePadBindings(const std::filesystem::path& file, const PadBindings& buttons);

/// Shows the Button Bindings dialog over owner, opened on the control type the game used last,
/// naming the pad's buttons as layout's prompts show them ("xbox", "switch", ...). Returns the
/// edited buttons on OK.
std::optional<PadBindings> EditPadBindings(HWND owner, const PadBindings& buttons, u32 control_type,
                                           std::string_view layout);

} // namespace Gr2Port::Launcher
