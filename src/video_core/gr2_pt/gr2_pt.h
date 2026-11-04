// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Vulkan {
class Instance;
class Rasterizer;
} // namespace Vulkan

// Passes of Gravity Rush 2's frame graph drawn by renderer code from the game's scene data, in
// place of the command stream the game records for them.
namespace Gr2Pt {

/// The NOP payload type of the marker packets the hooks write into the game's command buffers.
inline constexpr u32 MarkerPayload = 0x50543247;

/// Installs the hooks when module_base is the Gravity Rush 2 executable.
void Install(VAddr module_base);

/// Runs a marker packet on the command processor's thread; data starts at the payload type.
void OnMarker(Vulkan::Rasterizer& rasterizer, const u32* data, u32 dwords);

/// The rasterizer state the renderer reads.
struct RasterizerAccess {
    static const Vulkan::Instance& Instance(const Vulkan::Rasterizer& rasterizer);
};

} // namespace Gr2Pt
