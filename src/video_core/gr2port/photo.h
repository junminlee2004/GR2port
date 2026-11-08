// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/types.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/scene.h"

// The game's photos: each shot is drawn into a target of the screen drawing, then a game worker
// encodes that target's pixels as a JPEG. This hands the encoder the target GR2port drew.
namespace Gr2Port::Photo {

/// On the render thread, after the screen drawing: copies the frame's photo, if it drew one.
void Record(Memory& memory, vk::CommandBuffer cmd, const Scene::Header& header, u32 slot);

/// On the render thread, once the slot's commands have run: keeps the photo they copied.
void Collect(u32 slot);

/// On the encoder's thread: waits for a photo newer than the last one taken and gives its pixels,
/// rows of pitch bytes in B8G8R8A8 order. False while GR2fork's emulation runs, which reads the
/// game's own target, or when no photo comes.
bool Read(std::vector<u8>& pixels, u32& width, u32& height, u32& pitch);

} // namespace Gr2Port::Photo
