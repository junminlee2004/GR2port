// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>

#include "common/types.h"
#include "launcher/qt/input_config.h"

class QWidget;

namespace Gr2Port::Launcher {

/// Shows the Key Bindings dialog over owner, opened on the control type the game used last.
/// Returns the edited keys on OK.
std::optional<KeyBindings> EditKeyBindings(QWidget* owner, const KeyBindings& keys,
                                           u32 control_type);

/// Shows the Button Bindings dialog over owner, opened on the control type the game used last,
/// naming the pad's buttons as layout's prompts show them ("xbox", "switch", ...). Returns the
/// edited buttons on OK.
std::optional<PadBindings> EditPadBindings(QWidget* owner, const PadBindings& buttons,
                                           u32 control_type, std::string_view layout);

} // namespace Gr2Port::Launcher
