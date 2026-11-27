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

/// The upscaler's and frame generation's motion, R16G16 like View's but with characters moving
/// with the camera where CharacterMotion does not measure them, in the general layout after Record.
vk::Image MotionImage();
vk::ImageView MotionView();

/// With an upscaler or frame generation, on a GPU whose vertex shaders may write storage buffers,
/// the characters' depth pass measures the Character list's own motion for MotionImage.
bool CharacterMotion();
/// The image of that motion, in CharacterMotionFormat at the scene's size, which the pass clears to
/// UnmeasuredMotion and leaves in the shader read-only layout for Record.
inline constexpr vk::Format CharacterMotionFormat = vk::Format::eR16G16Sfloat;
inline constexpr float UnmeasuredMotion = 4096.0f;
vk::Image CharacterMotionImage();
vk::ImageView CharacterMotionView();
/// The device address of the clip history's half for the packet frame, which keeps the
/// Character list's clip positions for the next frame.
u64 ClipHistory(u64 frame);

} // namespace Gr2Port::Velocity
