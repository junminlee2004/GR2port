// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The volumetric fog, on the render thread: the game's cloud maps, the fog's density, darkness and
// sun shadow in cells over the view, the light they scatter toward the eye, and the pass that lays
// it over the scene.
namespace Gr2Port::Fog {

/// After Textures::Start and Shadows::Start: creates the volumes, sets and pipelines. Without
/// storage images of 8-bit formats the fog stays off. Without the fog_shadows setting the sun's
/// shadow pass is not made and the sun lights every cell.
bool Start(const Device& device, Memory& memory, vk::Format scene_format);

/// With the GPU idle: points the apply pass at the scene's depth.
void Resize(vk::ImageView depth);

/// The fog's scattered light for shaders to sample in the general layout, null when it is off.
vk::ImageView ScatteredView();

/// Outside rendering, after Shadows::Record: copies the game's noise once it exists, makes the
/// cloud maps again when their inputs changed, then fills the fog's cells for the packet at
/// address. Leaves the fog for fragment shaders to sample.
void RecordVolumes(vk::CommandBuffer cmd, const Scene::Header& header, u64 address);

/// Inside a rendering into the scene's colour, with the scene's depth readable: lays the fog over
/// the scene.
void RecordApply(vk::CommandBuffer cmd, u64 address, vk::Extent2D extent);

/// Once per report: logs the fog's state and the packet's fog values.
void Log(const u8* packet);

} // namespace Gr2Port::Fog
