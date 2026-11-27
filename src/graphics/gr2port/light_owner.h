// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The particle engine's light gathering. Once a frame on the draw thread FUN_00768940 walks the
// effects that carry lights and, for each light, reads a byte of the object at the effect's +0x10.
// That object can be missing there, and the read of address 0x71 then ends the game. The guard
// skips such a light for that frame and keeps what the effect held, for the log.
namespace Gr2Port::LightOwner {

/// Hooks the read of the effect's object.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Draw thread: logs the lights skipped since the start, when there are more than last time.
void Report();

} // namespace Gr2Port::LightOwner
