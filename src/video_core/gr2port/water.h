// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
struct Image;
} // namespace Gr2Port

// G2_BG_Water, drawn with the game's own programs after the glass: it reads the scene's fogged copy
// and a copy of its depth, and writes the scene's colour and depth.
namespace Gr2Port::Water {

/// After Textures::Start and Shadows::Start: creates the set and starts building the pipeline on
/// its own thread, since the programs are large. The draws stay off until it is ready.
bool Start(const Device& device, Memory& memory, vk::Format scene_format, vk::Format depth_format);

/// With the GPU idle: creates the depth's copy at extent, and points the programs at background.
bool Resize(vk::Extent2D extent, vk::ImageView background);

/// Outside rendering, with the scene's colour and depth as attachments and the fogged copy
/// readable: draws the water of the packet at address, which is at offset in buffer. Leaves the
/// scene's colour and depth as attachments.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, vk::ImageView scene, const Image& depth, vk::Extent2D extent);

} // namespace Gr2Port::Water
