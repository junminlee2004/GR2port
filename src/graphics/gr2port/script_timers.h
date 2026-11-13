// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Timers of the game's scripts that count calls. A script function that runs once a frame and
// adds a constant to a counter keeps its speed of 30 frames a second: each frame the constant
// becomes its value times the game step in thirtieths of a second.
namespace Gr2Port::ScriptTimers {

/// Hooks the game's Lua function loader and its release of a function. Returns whether both
/// hooks went in.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Once a frame, any thread: gives every loaded timer's constant the frame's value.
void OnFrame();

} // namespace Gr2Port::ScriptTimers
