// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// A layout animation's clock counts whole ticks of 1/3000 s and rounds each frame's time to a
// whole count. Above 30 fps a frame is a few ticks long, so the rounding changes the animation's
// speed: the 8.33 ticks of a 360 fps frame become 8, which is 4% slow, and the 12.5 ticks of a
// 240 fps frame become 13, which is 4% fast. The rounding takes the fps patch's running share of a
// 30 fps frame in place of one half, so the count follows the real time at every frame rate.
//
// A comic page starts its caption, which on a chapter's title page is the title, with a speed of
// 1/30 s a frame, and the fps patch puts the frame's time there. An animation's step already takes
// its speed times the frame's share of a 30 fps frame, so the caption would run slower by that
// share again: a title's fade-in of 1 s would take 2 s at 60 fps, and 8 s at 360 fps, where the
// step of 0.69 ticks counts as one. The caption takes the game's speeds.
namespace Gr2Port::LayoutClock {

/// Hooks the rounding of a layout animation's step and the two speeds of a comic page's caption.
/// Returns whether all hooks went in.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::LayoutClock
