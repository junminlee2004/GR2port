// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "graphics/gr2port/device.h"
#include "graphics/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The game's vegetation. Grass: a compute pass grows every instance's tufts into blades in one
// buffer, and one draw shades them all into the world's surfaces. Trees, the game's Branch system:
// a compute pass sways each instance's copy of its curves. A multi-draw shades them as ribbons, and
// each curve's patch tessellates into isolines, its leaves, which a geometry shader turns into
// quads, drawn into the world's surfaces and into the shadow cascades.
namespace Gr2Port::Vegetation {

/// After Textures::Start: creates the blades' buffer and the pipelines. Without tessellation and
/// geometry shaders no leaves are drawn.
bool Start(const Device& device, Memory& memory, std::span<const vk::Format> surface_formats,
           vk::Format depth_format, vk::Format shadow_format);

/// After Deformers::Record, outside rendering: sways the branches and grows the blades of the
/// packet at address, and makes them visible to vertex shaders and indirect draws.
void RecordKernels(vk::CommandBuffer cmd, const Scene::Header& header, u64 address);

/// Inside the world's surface rendering, with its viewport: draws the grass, the branches and the
/// leaves of the packet at address, which is at offset in buffer.
void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                    vk::Buffer buffer, u64 offset);

/// Inside a cascade's rendering, with its viewport and depth bias: draws the leaves' shadows.
void RecordShadows(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                   vk::Buffer buffer, u64 offset, u32 cascade);

} // namespace Gr2Port::Vegetation
