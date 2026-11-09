// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
struct Image;
} // namespace Gr2Port

// The deferred world, on the render thread: the surfaces of the game's world materials (albedo,
// normal and material images), the screen-space ink lines and ambient occlusion drawn from them,
// and the pass that lights them from the depth into the scene's colour.
namespace Gr2Port::World {

/// The surfaces: albedo with the hatching in alpha, the packed normal, and roughness, metallic,
/// translucency and emissive.
inline constexpr std::array<vk::Format, 3> SurfaceFormats{
    vk::Format::eR8G8B8A8Srgb,
    vk::Format::eR16G16Sfloat,
    vk::Format::eR8G8B8A8Unorm,
};

/// After Textures::Start and Shadows::Start: creates the pipelines and the surfaces' set.
bool Start(const Device& device, Memory& memory, vk::Format depth_format, vk::Format scene_format);

/// With the GPU idle: creates the surface images at extent. depth is the scene's depth, for the
/// light pass to sample; character_depth holds the characters' depth alone, for the occlusion.
bool Resize(vk::Extent2D extent, vk::ImageView depth, vk::ImageView character_depth);

/// After Resize: the view of the albedo surface.
vk::ImageView AlbedoView();

/// Outside rendering, with depth ready as an attachment: clears the surfaces and depth and, when
/// drawn, draws the world's draws and decals of the packet at address, which is at offset in
/// buffer. Leaves the surfaces readable and depth an attachment.
void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                    bool drawn, u64 address, vk::Buffer buffer, u64 offset, const Image& depth);

/// Outside rendering, with the surfaces and depth readable: draws the world's ink lines from them.
void RecordLines(vk::CommandBuffer cmd, u64 address);

/// After RecordLines, which starts the frame's history images, with the characters' depth
/// readable: draws the world's ambient occlusion from the depths and normals.
void RecordOcclusion(vk::CommandBuffer cmd, u64 address);

/// Inside a rendering into the scene's colour, after RecordOcclusion: lights the surfaces.
void RecordLight(vk::CommandBuffer cmd, u64 address);

} // namespace Gr2Port::World
