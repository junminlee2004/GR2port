// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "graphics/gr2port/scene.h"

// The capture of the game's particle system, on the game's draw thread.
namespace Gr2Port::ParticleCapture {

/// At the entry of the manager's first step of a frame: notes changes to the effect library and
/// takes the curves the game is about to bake. fx is the game's particle manager.
void OnSetup(VAddr fx);

/// After the manager's last step of a frame, when its list of draws exists: writes the particle
/// blocks of the packet and their numbers to header. view is the frame's view.
void Write(VAddr fx, VAddr view, u8* packet, Scene::Header& header);

/// When the engine's manager has filled one effect of the simulation's tables, FUN_00763370:
/// effect is the effect's number, record its record in the table of effects, owners the table of
/// owners or null.
void OnEffectFilled(u32 effect, VAddr record, VAddr owners);

/// When manager, the engine's, has filled the simulation's tables, FUN_00766b70.
void OnTablesFilled(VAddr manager);

/// In place of the memset with which FUN_00766b70 zeros a table of the simulation before it fills
/// it: when the table holds zeros but for the lines its last fill wrote, zeros those lines alone.
void* PS4_SYSV_ABI ClearTable(void* table, s32 value, u64 bytes);

/// When the engine frees the memory of a buffer, FUN_00771390: a table found at an address since
/// may be another one.
void OnBufferFreed();

} // namespace Gr2Port::ParticleCapture
