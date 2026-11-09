// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

// The characters' ink lines, the game's G2_CH_Contour pass: a geometry shader turns the silhouette
// edges of each mesh's triangles with adjacency into ribbons, blended over the scene.
namespace Gr2Port::Contours {

/// Creates the pipeline when the device has geometry shaders. Without them the lines are not drawn.
bool Start(const Device& device, vk::Format color_format, vk::Format depth_format);

/// Inside the scene's forward rendering, with the index buffer of meshes: draws the packet's
/// outlines.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
            u64 offset, vk::Extent2D extent);

} // namespace Gr2Port::Contours
