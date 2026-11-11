// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"
#include "graphics/gr2port/scene.h"

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

/// With the GPU idle: points the system at the scene's depth and the world's albedo surface, and
/// makes its copies of them.
bool Resize(vk::ImageView depth, vk::ImageView albedo, vk::Extent2D extent);

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

/// Outside rendering, after the simulation and with the albedo surface Resize named readable:
/// tints each object by the albedo under it in the last frame's quarter copy, as the game's
/// ExtractEmitterColor does, then copies this frame's albedo for the next, as BackupColorBuffer.
void RecordEmitterColors(vk::CommandBuffer cmd, const Scene::Header& header, u64 address);

/// The groups of passes the scene draws: the fog's low path draws its opaque pass with the opaque
/// draws and its others after TransparentBg, before the fog; the high path draws after the fog.
enum class Group {
    LowOpaque,
    LowTransparent,
    AfterFog,
};

/// Whether the packet holds draws of group's passes.
bool HasDraws(const Scene::Header& header, const u8* packet, Group group);

/// Outside rendering, with the scene's colour an attachment and depth readable: draws the items
/// of the packet at offset in buffer of group's passes, pass by pass in the game's order, and
/// leaves depth readable. With half_resolution_particles, AfterFog's draws that leave the depth
/// alone draw at half the scene's size and blend into it.
void RecordDraws(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
                 vk::Buffer buffer, u64 offset, vk::ImageView scene, const Image& depth,
                 vk::Extent2D extent, Group group);

} // namespace Gr2Port::Particles
