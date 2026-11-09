// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
struct Image;
} // namespace Gr2Port

// The game's metaballs, on the render thread: G2_BG_Metaball draws spheres into the world's
// surfaces that its vertex program pulls toward their neighbours, then SurfaceBlur blurs their
// normals across and down, where the game's stencil marks them, so they light as one surface. A
// mask the draws write stands for that stencil.
namespace Gr2Port::Metaballs {

/// After Textures::Start: creates the pipelines.
bool Start(const Device& device, Memory& memory, vk::Format depth_format);

/// With the GPU idle: creates the mask and the blur's image at extent; normal is the world's
/// normal surface.
bool Resize(vk::Extent2D extent, vk::ImageView normal);

/// Between renderings of the world's surfaces, with the surfaces' attachments and the depth's set
/// to load: draws the packet's metaballs into them and marks their pixels.
void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                    vk::Buffer buffer, u64 offset,
                    std::span<const vk::RenderingAttachmentInfo> colors,
                    const vk::RenderingAttachmentInfo& depth);

/// Outside rendering, with normal readable after the world's surfaces: blurs the metaballs'
/// normals as the game's SurfaceBlur does, and leaves normal readable.
void RecordBlur(vk::CommandBuffer cmd, const Scene::Header& header, const Image& normal);

} // namespace Gr2Port::Metaballs
