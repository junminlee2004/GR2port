// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// The passes that give a frame GR2's look, on the render thread: the meter that sets the exposure,
// and the game's Tonemap and Composite programs, which turn the scene's HDR colour into the
// picture.
namespace Gr2Port::Look {

inline constexpr vk::Format SceneFormat = vk::Format::eR16G16B16A16Sfloat;

/// Creates the pipelines and the meter.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: creates the scene's colour image at extent, and the picture's pipeline for
/// picture_format.
bool Resize(vk::Extent2D extent, vk::Format picture_format);

vk::Image SceneImage();
vk::ImageView SceneView();

/// After the scene's rendering, with the scene's colour readable: measures it and sets the
/// exposure.
void RecordMeter(vk::CommandBuffer cmd, u64 packet_address);

/// Inside the picture's rendering: draws the scene's colour as the picture.
void RecordComposite(vk::CommandBuffer cmd, u64 packet_address);

/// Outside rendering, once per report: logs the frame's look and the meter's last read state, and
/// reads the state again.
void LogAndReadMeter(vk::CommandBuffer cmd, const u8* packet);

} // namespace Gr2Port::Look
