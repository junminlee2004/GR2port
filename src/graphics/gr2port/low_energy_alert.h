// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The alert that sounds while Kat's gravity energy is low plays at the volume that the Low Energy
// Alert slider of the System Menu's Sound page sets, from silent to twice the game's own.
namespace Gr2Port::LowEnergyAlert {

/// Hooks the alert's sound, with the volume of the settings.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// Any thread: the alert's volume as the slider holds it, from 0, silent, over 50, the game's own,
/// to 100, twice the game's own, from the next time the alert sounds.
void Set(u32 position);

} // namespace Gr2Port::LowEnergyAlert
