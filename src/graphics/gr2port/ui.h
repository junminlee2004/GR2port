// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"
#include "graphics/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The game's screen drawing, on the render thread: an image for each of the game's targets, which
// include its back buffers, the screen draws into them, and the frame's back buffer on the window.
namespace Gr2Port::Ui {

/// The format of the game's targets. The game's Composite program encodes sRGB itself.
inline constexpr vk::Format TargetFormat = vk::Format::eR8G8B8A8Unorm;
/// The format of the gain image beside the back buffer: the world's colour as Composite drew it,
/// and in alpha the factor that takes its light to the HDR picture's.
inline constexpr vk::Format GainFormat = vk::Format::eR16G16B16A16Sfloat;

/// After Textures::Start: creates the samplers.
bool Start(const Device& device, Memory& memory);

/// After Start: whether the screen drawing keeps its 16:9 shape in the middle of the screen, as the
/// chosen aspect asks and the device allows. Otherwise a screen that is not 16:9 stretches it.
bool FitsScreen();

/// With the GPU idle: creates the pipeline that draws the back buffer on the window. hdr_white is
/// 0 for an SDR window, else the HDR window's paper white in units of 10000 nits.
bool Resize(vk::Format window_format, float hdr_white);

/// Outside rendering, before the frame's other calls here: creates the images of new targets and
/// copies the textures that change each frame again.
void Prepare(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 frame);

/// Outside rendering: begins a rendering into the target the world's picture goes to, and returns
/// its size; nothing and a size of 0 when it has no image. If hdr is true and that target is the
/// back buffer, the target's gain image is the rendering's second attachment.
vk::Extent2D BeginWorld(vk::CommandBuffer cmd, const Scene::Header& header, bool hdr);

/// Ends the rendering BeginWorld began and leaves the target for shaders to read.
void EndWorld(vk::CommandBuffer cmd);

/// Whether this frame's world went into the back buffer with its gain image, which the HDR window's
/// present reads.
bool WorldGain();

/// A target's size, 0 by 0 when the renderer has no image for it.
vk::Extent2D TargetExtent(u32 target);

/// A target's image, null when the renderer has none for it.
vk::Image TargetImage(u32 target);

/// After Record: copies a target's pixels to buffer, rows of 4 bytes per pixel in RGBA order.
void CopyTarget(vk::CommandBuffer cmd, u32 target, vk::Buffer buffer);

/// Outside rendering: draws the screen drawing of the packet at offset in buffer, mapped at
/// address.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
            vk::Buffer buffer, u64 offset);

/// Inside the window's rendering: draws the frame's back buffer at its own shape, centred in
/// extent. If generated is true, it draws frame generation's picture of the back buffer's size in
/// its place.
void RecordPresent(vk::CommandBuffer cmd, const Scene::Header& header, vk::Extent2D extent,
                   bool generated = false);

} // namespace Gr2Port::Ui
