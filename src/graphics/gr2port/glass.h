// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"
#include "graphics/gr2port/scene.h"

namespace Gr2Port {
class Memory;
struct Image;
} // namespace Gr2Port

// The game's G2_BG_Glass, after the scene's forward draws and before its fog. A copy of the scene's
// colour takes the fog, as the game's BackupColorBuffer2 does, and each pane refracts that copy.
// Without glass refraction the panes blend over the scene in two passes and read no copy.
namespace Gr2Port::Glass {

/// After Textures::Start and Shadows::Start: creates the pipelines the glass refraction setting
/// chooses.
bool Start(const Device& device, Memory& memory, vk::Format scene_format, vk::Format depth_format);

/// With the GPU idle: creates the scene's copy at extent.
bool Resize(vk::Extent2D extent);

/// The scene's copy, for what else refracts it. Readable after Prepare.
vk::ImageView BehindView();

/// Outside rendering, on the first frame after a resize: leaves the copy clear and readable.
void Prepare(vk::CommandBuffer cmd);

/// Outside rendering, with the scene's colour and depth as attachments: when the packet at address,
/// which is at offset in buffer, has water or an effect that reads the copy, or glass with
/// refraction, copies the scene; then draws the panes into it. Leaves the scene's colour and depth
/// as attachments.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, const Image& depth, vk::Extent2D extent);

} // namespace Gr2Port::Glass
