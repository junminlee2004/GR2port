// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>

#include "core/prompt_packs/keyboard_prompts.h"
#include "input/pad_bindings.h"

// Pad button prompts that follow the player's button bindings. When a game button has another pad
// button than by default, the prompts that show it are drawn again on the keyboard pack's blank
// textures with the bound button's name or mark as the pad's own prompts show it.
namespace PromptPacks::Pad {

/// The slot buttons ("cross", "R2", "touchpad", ...) whose game button has another pad button
/// than by default.
std::set<std::string> Changed(const Input::PadBindings::Map& buttons);

/// Each slot button's label on layout's pad ("playstation", "xbox", "switch", "steamdeck",
/// "steamcontroller"): the bound pad button for a game button, the pad's own for the rest.
std::map<std::string, Keyboard::Label> Labels(std::string_view layout,
                                              const Input::PadBindings::Map& buttons);

} // namespace PromptPacks::Pad
