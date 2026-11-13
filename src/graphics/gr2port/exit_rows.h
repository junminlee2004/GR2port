// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// An Exit to Desktop row in two of the game's menus: below Return to Title on the pause menu's
// Save / Load page, and at the bottom of the title menu's list of either story. Each row is one
// more of the game's own menu buttons, made with the list's rows and placed one row below the
// last of them; it is ticked and drawn right after one of the game's rows. The selection moves
// onto the row and off it ahead of the menu's own input code: before the Save / Load page's
// frame, and in the title menu's frame right after it reads the pad. Choosing the row asks
// through the game's own Yes / No dialog, and Yes closes the game's window. The rows' texts are
// records made in the form of the game's text bank, which the game's text lookup returns for the
// rows' keys; they are in the game's language when the language tables have it.
namespace Gr2Port::ExitRows {

/// Adds the rows to the game's menus.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::ExitRows
