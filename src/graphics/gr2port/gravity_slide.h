// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The gravity slide's steering. Each frame the slide moves its steering a fraction of the way to
// the right stick, 0.04 for the normal style, and eases it back by an amount: at the PS4's 30
// frames a second the steering takes about two seconds to follow the stick. Both steps take the
// game step's time, so the steering follows the stick as fast at every frame rate.
namespace Gr2Port::GravitySlide {

/// Hooks the two steps. Returns whether both hooks went in.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::GravitySlide
