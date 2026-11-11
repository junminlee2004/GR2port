// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/types.h"

// Keyboard button prompts that follow the player's bindings. The keyboard pack's members are the
// game's prompt textures with every key face left blank, and its slots.json says where each label
// goes, how big it is and how it is drawn. When the game starts, the key bound to each PlayStation
// button is drawn there: key names in the game's own DIN Round Bold, the mouse buttons, the wheel,
// Space, Shift and the arrows as symbols.
namespace PromptPacks::Keyboard {

/// A key as a prompt shows it: a name, or a symbol of keyboard_symbols.h.
struct Label {
    std::string text;
    std::string symbol;
    // The colour of a letter that the pad colours, as the Xbox face buttons; it replaces the
    // slot's own on the face buttons' prompts.
    std::optional<std::array<u8, 3>> rgb;
};

/// The label of each slot button ("cross", "R2", "Lstick", "touchpad", "swipe_up", ...) from the
/// player's input config. Buttons with no key bound show a dash.
std::map<std::string, Label> BoundLabels();

/// Draws labels, by slot button, into the members of the keyboard pack, keyed by lower-case
/// archive and member path, as slots_json describes; font is the game's DIN Round Bold. Members the
/// slots name that are missing are left out, and buttons without a label show a dash.
///
/// With only, just the textures with a slot of those buttons are drawn, each on its copy in blank
/// (the keyboard pack's members); the other textures of members stay as they are.
void Compose(const std::filesystem::path& slots_json, std::span<const u8> font,
             const std::map<std::string, Label>& labels,
             std::map<std::pair<std::string, std::string>, std::vector<u8>>& members,
             const std::set<std::string>* only = nullptr,
             const std::map<std::pair<std::string, std::string>, std::vector<u8>>* blank = nullptr);

} // namespace PromptPacks::Keyboard
