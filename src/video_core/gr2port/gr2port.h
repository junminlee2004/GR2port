// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// A renderer for Gravity Rush 2 that draws the game's scene with its own device, memory and
// shaders, apart from the emulated GPU.
namespace Gr2Port {

/// Installs the hooks when module_base is the Gravity Rush 2 executable.
void Install(VAddr module_base);

} // namespace Gr2Port
