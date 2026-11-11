// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// There is no GPU emulation: no renderer draws the game's command buffers and there is no command
// processor. Gr2Port::Submits walks the game's submits for their fences and flips, and the game's
// per-item draw loop does not run. Nor do the draws and dispatches the frame graph asks for: they
// are for the world's picture, which nothing reads. The screen drawing after the frame graph is
// written as the game writes it, because UiCapture reads it.
namespace Gr2Port::EmulatedGpu {

/// When the executable loads, with its base address. Stubs are written by c within capacity bytes
/// of its buffer.
void Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// On the game's draw thread: if running is true, the frame graph begins; otherwise it has
/// returned.
void SetFrameGraphRunning(bool running);

/// Any thread: whether a GPU emulation runs. None does.
bool On();

} // namespace Gr2Port::EmulatedGpu
