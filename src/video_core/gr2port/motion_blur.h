// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// MotionBlur: the game's blur along the screen's motion, after the bloom, with its own programs.
namespace Gr2Port::MotionBlur {

/// Creates the sets and the pipelines; scene_format is the scene's colour.
bool Start(const Device& device, Memory& memory, vk::Format scene_format);

/// With the GPU idle: creates the images at extent. velocity is read in the general layout, the
/// scene's and the characters' depth in the depth read-only layout; scene is the colour it blurs.
bool Resize(vk::Extent2D extent, vk::ImageView velocity, vk::ImageView depth,
            vk::ImageView character_depth, vk::Image scene, vk::ImageView scene_view);

/// Outside rendering, after the bloom, with the scene's colour in the shader read layout: blurs it
/// with the constants of the packet at address, and leaves it in that layout.
void Record(vk::CommandBuffer cmd, u64 address);

} // namespace Gr2Port::MotionBlur
