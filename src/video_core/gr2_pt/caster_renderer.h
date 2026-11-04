// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2_pt/marker.h"

namespace Vulkan {
class Rasterizer;
}

// Draws the batched shadow casters on the command processor's thread: the vertex programs pull
// their streams and matrices through device addresses, and one indirect draw call covers every
// caster that shares a pipeline, a cull mode and an index buffer. One frame in an interval also
// draws its casters into a copy of the shadow map, which is read back and compared with the
// game's own draws.
namespace Gr2Pt::CasterRenderer {

/// Builds the renderer once and lets the draw thread batch casters.
void Prepare(Vulkan::Rasterizer& rasterizer);

void OnSnapshot(Vulkan::Rasterizer& rasterizer, const Marker::Words& words);
void OnBatch(Vulkan::Rasterizer& rasterizer, const Marker::Words& words);

} // namespace Gr2Pt::CasterRenderer
