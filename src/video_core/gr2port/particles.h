// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
struct Image;
} // namespace Gr2Port

// The game's particle system. Its compute programs run on the renderer's own copies of the pool
// each frame, from the packet's input, and its draws read the pool.
namespace Gr2Port::Particles {

/// After Textures::Start: creates the sets and starts building the pipelines on their own thread,
/// since the programs are large. The system stays off until they are ready.
bool Start(const Device& device, Memory& memory, vk::Format scene_format, vk::Format depth_format);

/// Any thread: takes a copy of a curve for the atlas: columns columns of the game's 64 rows, at
/// texels with pitch texels a row, for the descriptions from destination on.
void QueueCurve(u32 destination, u32 columns, VAddr texels, u32 pitch);

/// With the GPU idle: points the system at the scene's depth and makes its copy of the scene.
bool Resize(vk::ImageView depth, vk::Extent2D extent);

/// Whether the scene's depth holds a frame and is readable before this frame's world.
bool DepthReady();

/// Outside rendering, with the last frame's depth readable: copies what changed in the effect
/// library and the curves, and runs the simulation for the packet at address, which is at offset
/// in buffer.
void RecordSimulation(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                      u64 address, vk::Buffer buffer, u64 offset);

/// Inside SelfTranslucent's rendering, with its viewport set: draws the items of its three
/// passes, each where the stencil holds its value.
void RecordSelfTranslucent(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                           u64 address, vk::Buffer buffer, u64 offset);

/// Inside a cascade's rendering, with its viewport and depth bias set: draws the items of that
/// cascade.
void RecordShadows(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                   u64 address, vk::Buffer buffer, u64 offset, u32 cascade);

/// Inside the rendering of the world's surfaces, with its viewport set: draws the mesh particles.
void RecordSurfaces(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                    u64 address, vk::Buffer buffer, u64 offset);

/// Outside rendering, between the world's surfaces and the mesh particles, with depth, the image of
/// the view Resize was given, an attachment: copies it as the game's BackupCollisionBuffer does for
/// the next frame's collisions, and leaves it an attachment.
void RecordCollision(vk::CommandBuffer cmd, const Image& depth);

/// Outside rendering, with the scene's colour an attachment and depth readable: draws the items
/// of the packet at offset in buffer, pass by pass in the game's order, and leaves depth readable.
void RecordDraws(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
                 vk::Buffer buffer, u64 offset, vk::ImageView scene, const Image& depth,
                 vk::Extent2D extent);

} // namespace Gr2Port::Particles
