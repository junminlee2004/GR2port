// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// ShadowTrace: the game's screen-space shadows of the point lights, which its LightPass reads
// as SSShadow, drawn with its own program.
namespace Gr2Port::ShadowTrace {

/// After PointLights::Start: creates the set and the pipeline.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: creates the shadow image at extent; depth is the scene's, read in the
/// depth read-only layout.
bool Resize(vk::Extent2D extent, vk::ImageView depth);

/// The shadow image, in the shader read layout after Record, its red channel the shadow.
vk::ImageView View();

/// Outside rendering, with the scene's depth readable: draws the shadows of the packet at
/// address when the game traced them, and leaves the image for the light pass to read.
void Record(vk::CommandBuffer cmd, u64 address, bool traced);

} // namespace Gr2Port::ShadowTrace
