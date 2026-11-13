// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Kat's water pass FUN_01527a30 makes a splash where her feet move in the water, and in one
// other path where a single point of hers does. A point whose speed is 2 or less adds to a sum
// and makes a splash when the sum is full, at most once in 0.2 s of game time. A faster point
// makes a splash on every frame, a rate made for 30 fps: at 240 fps it makes eight times as
// many, and they pile up into a white cloud. The stubs here let a fast point make a splash once
// in each 30 fps frame of game time, which at 30 fps is every frame.
namespace Gr2Port::WaterSplashes {

/// Limits the splashes of Kat's fast points in the water to the rate of a 30 fps game.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::WaterSplashes
