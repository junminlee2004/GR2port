// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The layout of a frame's scene packet in the scene ring.
namespace Gr2Pt::Scene {

/// The start of each packet.
struct Header {
    u64 frame;
};

} // namespace Gr2Pt::Scene
