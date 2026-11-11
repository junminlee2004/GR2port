// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "common/types.h"
#include "launcher/qt/input_config.h"

class QWidget;

namespace Gr2Port::Launcher {

/// Shows the Key Bindings dialog over owner, opened on the control type the game used last.
/// Returns the edited keys on OK.
std::optional<KeyBindings> EditKeyBindings(QWidget* owner, const KeyBindings& keys,
                                           u32 control_type);

} // namespace Gr2Port::Launcher
