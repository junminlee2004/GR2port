// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Hair is a chain of nodes that the game steps once a frame. When the air moves along a chain
// faster than the chain's limit, its step FUN_01501b20 makes the hair flutter: it pushes the
// middle node sideways by a wave and advances the wave's phase by an amount made for a 30 fps
// frame. The wave therefore runs faster as the frame rate rises, most of all while Kat floats,
// when the game moves the air along her hair on every frame. The stub here multiplies the
// advance by the game step's time in 30 fps frames, which is 1 at 30 fps. The step's other
// values stay as they are.
namespace Gr2Port::HairFlutter {

/// Scales the advance of the hair's flutter by the game step's time.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::HairFlutter
