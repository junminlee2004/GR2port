// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/gr2port/scene.h"

// The capture of the game's particle system, on the game's draw thread.
namespace Gr2Port::ParticleCapture {

/// At the entry of the manager's first step of a frame: notes changes to the effect library and
/// takes the curves the game is about to bake. fx is the game's particle manager.
void OnSetup(VAddr fx);

/// After the manager's last step of a frame, when its list of draws exists: writes the particle
/// blocks of the packet and their numbers to header. view is the frame's view.
void Write(VAddr fx, VAddr view, u8* packet, Scene::Header& header);

} // namespace Gr2Port::ParticleCapture
