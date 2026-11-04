// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The opaque shadow casters of the three ShadowCast passes: SubmitCategory's category 0 on the
// shadow view, one submission per cascade.
namespace Gr2Pt::ShadowPass {

/// Marker pass ids of the cascades.
inline constexpr u32 FirstPass = 0;
inline constexpr u32 Cascades = 3;

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Pt::ShadowPass
