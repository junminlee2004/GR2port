// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// The game's Velocity step, on the render thread: how far each pixel moved on the screen since the
// last frame, in clip space.
namespace Gr2Port::Velocity {

/// Creates the pipeline.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: creates the image at extent, to fill from the scene's and the characters'
/// depth.
bool Resize(vk::Extent2D extent, vk::ImageView depth, vk::ImageView character_depth);

/// Outside rendering: a drawn frame, with both depths readable, gets its motion, any other frame
/// holds still. Compute shaders can then read the image in the general layout.
void Record(vk::CommandBuffer cmd, bool drawn, u64 packet_address);

vk::ImageView View();

/// The upscaler's motion, R16G16 like View's but with characters moving with the camera, in the
/// general layout after Record.
vk::Image MotionImage();
vk::ImageView MotionView();

} // namespace Gr2Port::Velocity
