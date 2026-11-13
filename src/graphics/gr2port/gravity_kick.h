// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The gravity kick's sweep for the surface it lands on. Each frame the kick sweeps the path of
// that frame, the game step times Kat's velocity, less 0.3 m: at the PS4's 30 frames a second a
// kick of 20 m/s reaches 0.37 m ahead. A step shorter than 15 ms moves her less than 0.3 m, the
// sweep is skipped, and the kick lands on nothing: Kat flies on at the kick's speed and the
// collision of her body turns her aside. The sweep takes a step of at least a thirtieth of a
// second, so its reach is the PS4's at every frame rate.
namespace Gr2Port::GravityKick {

/// Hooks the sweep's step. Returns whether the hook went in.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::GravityKick
