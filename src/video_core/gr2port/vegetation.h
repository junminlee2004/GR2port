// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

// The trees' leaves, the game's Branch draws: each branch patch tessellates into isolines, its
// leaves, which a geometry shader turns into quads, drawn into the world's surfaces and into the
// shadow cascades.
namespace Gr2Port::Vegetation {

/// After Textures::Start: creates the pipelines when the device has tessellation and geometry
/// shaders. Without them no leaves are drawn.
bool Start(const Device& device, std::span<const vk::Format> surface_formats,
           vk::Format depth_format, vk::Format shadow_format);

/// Inside the world's surface rendering, with its viewport: draws the leaves of the packet at
/// address, which is at offset in buffer.
void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                    vk::Buffer buffer, u64 offset);

/// Inside a cascade's rendering, with its viewport and depth bias: draws the leaves' shadows.
void RecordShadows(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                   vk::Buffer buffer, u64 offset, u32 cascade);

} // namespace Gr2Port::Vegetation
