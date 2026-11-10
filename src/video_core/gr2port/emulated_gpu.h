// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {
struct Liverpool;
} // namespace AmdGpu

// GR2fork's GPU emulation does not run: its renderer is not created and there is no command
// processor. Gr2Port::Submits walks the game's submits for their fences and flips, and the game's
// per-item draw loop does not run. GR2port's own frames do not depend on any of it.
namespace Gr2Port::EmulatedGpu {

/// When the executable loads, with its base address.
void Install(VAddr module_base);

/// Any thread: whether GR2fork's renderer is created. It never is, so the emulator's window has
/// no swapchain and does not present.
bool Wanted();

/// From the rasterizer's constructor, after it is bound unless the settings ask for no GPU.
void Start(AmdGpu::Liverpool* liverpool);

/// Any thread: whether GR2fork's GPU emulation runs.
bool On();

} // namespace Gr2Port::EmulatedGpu
