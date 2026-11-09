// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
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

} // namespace Gr2Port::Launcher
