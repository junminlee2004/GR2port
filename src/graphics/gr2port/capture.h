// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The scene capture on the game's draw thread.
namespace Gr2Port::Capture {

/// At the frame graph's entry, with the view it draws: writes the frame's scene into the scene
/// ring.
void OnFrame(VAddr frame_graph, VAddr view);

/// At the entry of the particle manager's first step of a frame.
void OnParticleSetup(VAddr fx);

/// After the particle manager's last step of a frame: completes the frame's scene.
void OnParticleList(VAddr fx);

/// At the end of each frame of the game's draw thread, after its screen drawing: adds that to the
/// frame's scene, or to a packet of its own when the frame graph did not run, and publishes it.
void OnFrameEnd();

/// The frames the game has started.
u64 Frames();

/// Any thread: stops or restarts the capture, so that the game runs without the renderer's work.
void ToggleCapture();

} // namespace Gr2Port::Capture
