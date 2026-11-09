// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The game's screen drawing, on the render thread: an image for each of the game's targets, which
// include its back buffers, the screen draws into them, and the frame's back buffer on the window.
namespace Gr2Port::Ui {

/// The format of the game's targets. The game's Composite program encodes sRGB itself.
inline constexpr vk::Format TargetFormat = vk::Format::eR8G8B8A8Unorm;

/// After Textures::Start: creates the samplers.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: creates the pipeline that draws the back buffer on the window.
bool Resize(vk::Format window_format);

/// Outside rendering, before the frame's other calls here: creates the images of new targets and
/// copies the textures that change each frame again.
void Prepare(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 frame);

/// Outside rendering: begins a rendering into the target the world's picture goes to, and returns
/// its size; nothing and a size of 0 when it has no image.
vk::Extent2D BeginWorld(vk::CommandBuffer cmd, const Scene::Header& header);

/// Ends the rendering BeginWorld began and leaves the target for shaders to read.
void EndWorld(vk::CommandBuffer cmd);

/// A target's size, 0 by 0 when the renderer has no image for it.
vk::Extent2D TargetExtent(u32 target);

/// After Record: copies a target's pixels to buffer, rows of 4 bytes per pixel in RGBA order.
void CopyTarget(vk::CommandBuffer cmd, u32 target, vk::Buffer buffer);

/// Outside rendering: draws the screen drawing of the packet at offset in buffer, mapped at
/// address.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
            vk::Buffer buffer, u64 offset);

/// Inside the window's rendering: draws the frame's back buffer at its own shape, centred in
/// extent.
void RecordPresent(vk::CommandBuffer cmd, const Scene::Header& header, vk::Extent2D extent);

} // namespace Gr2Port::Ui
