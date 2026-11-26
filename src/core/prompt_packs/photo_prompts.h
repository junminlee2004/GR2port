// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "input/pad_bindings.h"

// The photo mode's prompts of Hide UI, Open Collection and Open Picture Roll, which follow their
// pad bindings (Input::PadBindings::PhotoFunctions). Each pad layout's pack draws the default
// bindings, L3 with R2, L1 and R1, into the photo mode's icons 19, 21 and 22: the two buttons'
// icons at one scale, joined by a plus. Another binding is drawn there the same way from the
// layout's own icons of its buttons, a back paddle as a shoulder key with the paddle's name.
namespace PromptPacks::Photo {

/// The members of the photo mode's icons that Compose reads and draws.
std::vector<std::string> Members();

/// Whether layout's prompts ("playstation", "xbox", "switch", "steamdeck", "steamcontroller")
/// show other buttons than photo binds: whether Compose draws anything.
bool Changed(std::string_view layout, const Input::PadBindings::Map& photo);

/// Whether a binding of photo names a back paddle.
bool UsesPaddle(const Input::PadBindings::Map& photo);

/// A member of the game's archive, by path, as the layout's pack leaves it.
using Read = std::function<std::optional<std::vector<u8>>(const std::string& path)>;

/// A shoulder key with text on it, as the photo mode's L1 icon: 128 x 128 RGBA8 rows.
using ShoulderKey = std::function<std::optional<std::vector<u8>>(const std::string& text)>;

/// The icons, by member path, of the functions whose binding layout's art does not show, drawn
/// again: the binding's buttons from read's icons, a back paddle as shoulder_key's key with its
/// name, and nothing for a function without a binding.
std::map<std::string, std::vector<u8>> Compose(std::string_view layout,
                                               const Input::PadBindings::Map& photo,
                                               const Read& read, const ShoulderKey& shoulder_key);

} // namespace PromptPacks::Photo
