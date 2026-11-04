// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2_pt/marker.h"

namespace Vulkan {
class Rasterizer;
}

// The cost of each marked pass on every thread that handles it: the game's draw thread, the
// command processor, the record thread and the GPU. Logs the averages at a fixed frame interval.
namespace Gr2Pt::PassTiming {

/// Runs on the command processor's thread at a pass marker.
void OnMarker(Vulkan::Rasterizer& rasterizer, const Marker::Words& words);

} // namespace Gr2Pt::PassTiming
