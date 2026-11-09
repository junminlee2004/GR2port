// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <SDL3/SDL_joystick.h>

// The pad that plays as player 1: the one picked in the launcher for this run, else the default
// gamepad of the settings, else the first pad connected.
namespace Input::SelectedPad {

/// The GUID of the pad for player 1; empty when no pad is picked.
std::string Guid();

/// Launcher: the pad for this run, by GUID; empty for the default.
void Set(std::string guid);

/// The GUID of a connected pad as a string of 32 hex digits.
std::string GuidOf(SDL_JoystickID id);

} // namespace Input::SelectedPad
