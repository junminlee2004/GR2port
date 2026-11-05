// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The scene capture on the game's draw thread.
namespace Gr2Pt::Capture {

/// At the frame graph's entry, with the view it draws: writes the frame's scene into the scene
/// ring.
void OnFrame(VAddr view);

/// The frames the game has started.
u64 Frames();

} // namespace Gr2Pt::Capture
