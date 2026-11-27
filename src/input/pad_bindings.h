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
// config as controller lines: "cross = r1" means R1 presses cross. A back paddle's line binds it
// beside the button's own pad button: "cross = lpaddle_high" means the upper left paddle presses
// cross too.
namespace Input::PadBindings {

/// Game button -> pad button, by the input config's names; empty for none.
using Map = std::map<std::string, std::string, std::less<>>;

/// The PlayStation buttons the game's functions use, by the input config's output names.
std::span<const std::string_view> GameButtons();

/// Whether the input config can bind name as a pad button.
bool IsPadInput(std::string_view name);

/// The back paddles by the input config's names: upper left, upper right, lower left, lower right.
std::span<const std::string_view> Paddles();

bool IsPaddle(std::string_view name);

/// Each button presses itself, and Back presses the touch pad.
Map Defaults();

/// A controller line without its comment and spaces, for one of those buttons: the button and
/// its pad button or back paddle, empty for unmapped. A press anywhere on the touch pad counts as
/// its centre.
std::optional<std::pair<std::string, std::string>> ParseLine(std::string_view compact);

/// The input config's controller lines for those buttons; the defaults when it has none. A
/// button's first line counts, and a later one where the first leaves it unbound. Back paddle
/// lines do not count.
Map Read(const std::filesystem::path& file);

/// Back paddle -> the button it presses, from the input config's controller lines. A paddle's
/// first line counts; a paddle left out presses none.
Map ReadPaddles(const std::filesystem::path& file);

/// The photo mode's functions with a pad binding of their own, by the input config's names: Hide
/// UI, Open Collection, Open Picture Roll and Rotate Camera. Each acts only while the camera is
/// out. The first three act on two pad buttons, the first held while the second is pressed
/// ("l3, r2"), or on a back paddle alone; Rotate Camera on one pad button or back paddle, held
/// while the right stick turns the camera ("l3").
std::span<const std::string_view> PhotoFunctions();

/// Whether inputs, compact ("l3,r2"), is the binding of function, a photo function: two different
/// pad buttons or a back paddle alone, or for Rotate Camera one pad button or back paddle.
bool IsPhotoBinding(std::string_view function, std::string_view inputs);

/// Whether function, a photo function, takes one button: Rotate Camera.
bool IsHeldAlone(std::string_view function);

/// The photo functions' bindings without their lines: the swipe combos' hold input, hold, with R2,
/// L1 and R1, and alone for Rotate Camera, compact. None when hold is empty.
Map PhotoDefaults(std::string_view hold = "l3");

/// A photo function's line without its comment and spaces: the function and its compact binding,
/// empty for unmapped.
std::optional<std::pair<std::string, std::string>> ParsePhotoLine(std::string_view compact);

/// The photo functions' bindings from the input config's lines, a function's last one; a function
/// without one has its default, from the config's swipe combo hold input.
Map ReadPhoto(const std::filesystem::path& file);

/// Whether a line without its comment and spaces is a photo function's, whatever its inputs.
bool IsPhotoLine(std::string_view compact);

} // namespace Input::PadBindings
