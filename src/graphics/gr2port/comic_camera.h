// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The camera of a 3D comic page. After a panel change the camera moves a fraction of the way to
// the new panel each frame, 0.2 by default, and from the 15th frame that fraction grows to 1 over
// 10 frames: at the PS4's 30 frames a second the move takes about 0.8 s. The frame counts and the
// fractions take the real frame's time, which slow motion and a stopped game clock leave alone, so
// the move takes as long at every frame rate. On a comic's last page the menu waits 20 frames for
// the panel's animation to start before it can end the comic, and that wait takes the same time at
// every frame rate too.
//
// The game starts a panel's animation once the camera's gaps to the panel are small. On a page
// that turns its own panels, as a chapter's title page does, the menu asks for the next panel on
// each frame with no animation playing, and a request during a move ends the move. A 30 fps
// frame's step of 0.2 leaves the title's camera 0.94 from its panel, under the 1 that starts the
// title's animation, and a 60 fps frame's shorter step leaves 1.05, which would let the next
// frame's request end the move before the title shows. The game's tests take the gaps a 30 fps
// frame's step would leave, so a move's first frame passes them when it does at 30 fps.
namespace Gr2Port::ComicCamera {

/// Hooks the move's frame counts, both of its fractions, its gaps and the last page's wait. They
/// read the fps patch's frame time, so they go in only above 30 fps. Returns whether all hooks
/// went in.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::ComicCamera
