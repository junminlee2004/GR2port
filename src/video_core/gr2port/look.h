// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// The passes that give a frame GR2's look, on the render thread: the sky behind the scene, the
// meter that sets the exposure, the bloom, and the game's Tonemap, Antialias and Composite
// programs, which turn the scene's HDR colour into the picture.
//
// The scene's alpha stands for the game's stencil bit 3, which keeps a pixel in the antialias
// program's history: the world and the sky write 1, the passes that clear the bit write 0.
namespace Gr2Port::Look {

inline constexpr vk::Format SceneFormat = vk::Format::eR16G16B16A16Sfloat;

/// After Textures::Start: creates the pipelines and the meter. depth_format is the scene's.
bool Start(const Device& device, Memory& memory, vk::Format depth_format);

/// With the GPU idle: creates the scene's colour image at extent, the bloom's images, and the
/// picture's pipeline for picture_format. depth is the scene's depth, for the bloom to read.
bool Resize(vk::Extent2D extent, vk::Format picture_format, vk::ImageView depth);

vk::Image SceneImage();
vk::ImageView SceneView();

/// Inside the scene's rendering, after its opaque draws: draws the sky where nothing is drawn yet.
void RecordSky(vk::CommandBuffer cmd, u64 packet_address);

/// After the scene's rendering, with the scene's colour readable: measures it and sets the
/// exposure.
void RecordMeter(vk::CommandBuffer cmd, u64 packet_address);

/// Outside rendering, with the scene's colour and depth readable: draws the game's bloom when the
/// header's bloom flags say it runs.
void RecordBloom(vk::CommandBuffer cmd, u32 bloom, u64 packet_address);

/// Outside rendering, with the scene's colour readable, after the bloom: tonemaps the scene and
/// runs the game's antialias program on the result.
void RecordAntialias(vk::CommandBuffer cmd, u64 packet_address);

/// Inside the picture's rendering, after RecordAntialias: draws the picture.
void RecordComposite(vk::CommandBuffer cmd, u64 packet_address);

/// Outside rendering, once per report: logs the frame's look and the meter's last read state, and
/// reads the state again.
void LogAndReadMeter(vk::CommandBuffer cmd, const u8* packet);

} // namespace Gr2Port::Look
