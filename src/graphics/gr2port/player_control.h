// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The player's controls. FUN_008f6430 runs them each frame, with the control type of the game's
// Controls option and the gravity style the player has. The keyboard's game functions follow both,
// and the control type seen last is kept for the keyboard prompts of the next start.
namespace Gr2Port::PlayerControl {

/// Observes the player's controls.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::PlayerControl
