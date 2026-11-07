// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace AmdGpu {
struct Liverpool;
} // namespace AmdGpu

namespace Vulkan {
class Rasterizer;
} // namespace Vulkan

// Turns GR2fork's GPU emulation off and on: without a rasterizer the command processor parses the
// game's commands, signals its fences and flips, and draws nothing. GR2port's own frames do not
// depend on it. GR2PORT_NO_FORK in the environment starts it off, and F3 toggles it, since menus
// exist only in GR2fork's window. While it is off the game's per-item draw loop does not run.
namespace Gr2Port::EmulatedGpu {

/// When the executable loads, with its base address.
void Install(VAddr module_base);

/// From the rasterizer's constructor, with the rasterizer bound unless the settings ask for no GPU.
void Start(AmdGpu::Liverpool* liverpool, Vulkan::Rasterizer* rasterizer);

/// Any thread: asks the command processor to change over between its commands.
void Toggle();

/// Any thread: whether GR2fork's GPU emulation runs.
bool On();

} // namespace Gr2Port::EmulatedGpu
