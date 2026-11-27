// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The particle engine's draw items. Once a frame on the draw thread FUN_00768940 walks the list of
// effects and writes a draw item for each of an effect's draw nodes, reading a byte of the
// effect's emitter, the object at the effect's +0x10, for each. The main thread ends an effect in
// its own update, in FUN_00775da0 or FUN_00775cb0: it clears that pointer first and marks the
// effect dead after. The draw thread tests the mark once for an effect and the two take no lock,
// so an effect that ends while its items are written has no emitter, and the read of address 0x71
// then ends the game. The guard skips such an item and keeps what the effect held, for the log.
namespace Gr2Port::EffectEmitter {

/// Hooks the read of the effect's emitter.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Draw thread: logs the items skipped since the start, when there are more than last time.
void Report();

} // namespace Gr2Port::EffectEmitter
