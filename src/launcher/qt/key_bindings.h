// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "common/types.h"
#include "launcher/input_config.h"

class QString;
class QWidget;

namespace Gr2Port::Launcher {

/// Shows the Key Bindings dialog over owner, opened on the control type the game used last, with
/// the photo functions' pad bindings from pad. Returns the edited keys on OK.
std::optional<KeyBindings> EditKeyBindings(QWidget* owner, const KeyBindings& keys,
                                           u32 control_type, const PadBindings& pad);

/// Shows the Button Bindings dialog over owner, opened on the control type the game used last,
/// naming the pad's buttons as layout's prompts show them ("xbox", "switch", ...). Returns the
/// edited buttons and back paddles on OK.
std::optional<PadBindings> EditPadBindings(QWidget* owner, const PadBindings& bindings,
                                           u32 control_type, std::string_view layout);

/// Waits over owner for up to three keys, or pad buttons when pad is set, held together until all
/// are released, and returns them as an input config line's inputs ("lctrl, t"). Empty when the
/// countdown ends first.
std::string CaptureCombination(QWidget* owner, const QString& function, bool pad);

/// The text of an input config line's inputs ("l3, r3" -> "L3 + R3"), with pad buttons named as
/// layout's prompts show them.
QString CombinationText(std::string_view inputs, bool pad, std::string_view layout);

} // namespace Gr2Port::Launcher
