// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The deferred world, on the render thread: the surfaces of the game's world materials (albedo,
// normal and material images), the screen-space ink lines and ambient occlusion drawn from them,
// and the pass that lights them from the depth into the scene's colour.
namespace Gr2Port::World {

/// After Textures::Start and Shadows::Start: creates the pipelines and the surfaces' set.
bool Start(const Device& device, Memory& memory, vk::Format depth_format, vk::Format scene_format);

/// With the GPU idle: creates the surface images at extent. depth is the scene's depth, for the
/// light pass to sample.
bool Resize(vk::Extent2D extent, vk::ImageView depth);

/// Outside rendering, with depth ready as an attachment: clears the surfaces and depth and, when
/// drawn, draws the world's draws of the packet at address, which is at offset in buffer. Leaves
/// the surfaces readable.
void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, bool drawn, u64 address,
                    vk::Buffer buffer, u64 offset, vk::ImageView depth);

/// Outside rendering, with the surfaces and depth readable: draws the world's ink lines from them.
void RecordLines(vk::CommandBuffer cmd, u64 address);

/// After RecordLines, which starts the frame's history images: draws the world's ambient
/// occlusion from the depth and normals.
void RecordOcclusion(vk::CommandBuffer cmd, u64 address);

/// Inside a rendering into the scene's colour, after RecordOcclusion: lights the surfaces.
void RecordLight(vk::CommandBuffer cmd, u64 address);

} // namespace Gr2Port::World
