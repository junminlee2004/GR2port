// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>
#include <string_view>

#include "common/types.h"

namespace Input {
class GameController;
}

// Gravity Rush 2's own functions, bound to keys by the input config's gr2_<function> lines. The
// game's Controls option picks one of five control types, Type A to Type E, which differ only in
// how the player evades and changes gravity style. A function's key does the same in every type: a
// function on the same pad input in every type binds that input, and Evade and the three styles
// press the pad inputs of the type the game uses as their key goes down.
namespace Input::GameFunctions {

/// The game's control types, Type A to Type E.
constexpr u32 TypeCount = 5;

/// The gravity styles, as the game numbers them.
enum class Style : u32 {
    Normal,
    Lunar,
    Jupiter,
};

struct Function {
    /// The input config's output name.
    std::string_view name;
    /// The function's name in the launcher, after the game's Controls page.
    std::string_view label;
    /// What it presses on the pad in each control type, as the launcher shows it.
    std::array<std::string_view, TypeCount> pad;
    /// The pad output it binds in every type, or empty when the type decides at each press.
    std::string_view output;
    /// Its key in GR2port's default input config, or empty for none.
    std::string_view key;
};

/// Every function, in the launcher's order.
std::span<const Function> Functions();

/// The function an input config output names, or null.
const Function* Find(std::string_view name);

/// Any thread: the control type the game uses, 0 to 4 for Type A to Type E.
u32 ControlType();

/// The player's control, each frame: the control type and the player's gravity style.
void SetGameState(u32 type, Style style);

/// Startup: the control type the game used last, until the game reports it.
void SetControlType(u32 type);

/// Input handling, when Evade's key goes down or up. The left stick's position, -127 to 127,
/// picks the direction of a touch pad flick.
void Evade(GameController* controller, bool pressed, s16 left_x, s16 left_y);

/// Input handling, when a style's key goes down or up. The sticks' vertical positions, -127 to
/// 127, are where they go back to when a stick tilt ends.
void ChangeStyle(GameController* controller, Style style, bool pressed, s16 left_y, s16 right_y);

} // namespace Input::GameFunctions
