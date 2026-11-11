// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// The pad buttons bound to the PlayStation buttons the game's functions use, kept in the input
// config as controller lines: "cross = r1" means R1 presses cross.
namespace Input::PadBindings {

/// Game button -> pad button, by the input config's names; empty for none.
using Map = std::map<std::string, std::string, std::less<>>;

/// The PlayStation buttons the game's functions use, by the input config's output names.
std::span<const std::string_view> GameButtons();

/// Whether the input config can bind name as a pad button.
bool IsPadInput(std::string_view name);

/// Each button presses itself, and Back presses the touch pad.
Map Defaults();

/// A controller line without its comment and spaces, for one of those buttons: the button and
/// its pad button, empty for unmapped. A press anywhere on the touch pad counts as its centre.
std::optional<std::pair<std::string, std::string>> ParseLine(std::string_view compact);

/// The input config's controller lines for those buttons; the defaults when it has none. A
/// button's first line counts, and a later one where the first leaves it unbound.
Map Read(const std::filesystem::path& file);

} // namespace Input::PadBindings
