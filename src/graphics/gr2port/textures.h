// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "graphics/gr2port/device.h"

namespace Xbyak {
class CodeGenerator;
}

namespace Gr2Port {
class Memory;
}

// The game's GNF textures (InitTextureFromGnf 0x1618ca0) converted into the renderer's images when
// the game builds them: detiled on the renderer's GPU, block-compressed formats kept. Each texture
// object holds its handle at +0x6c, which no game code reads or writes. Shaders sample every
// texture through one array indexed by descriptor, as a 2D texture or a cube map by the texture's
// kind; descriptor 0 is a white 2D texture.
//
// Streaming keeps the images within the device's memory budget: under it, textures the game did
// not draw for a while keep only their small levels, and the largest levels of the others go
// first. The game keeps every texture's data in its memory, so a texture gets larger levels again
// by an upload from it. A texture moves between its handle's descriptor and a twin one, so frames
// in flight keep sampling the image they started with.
namespace Gr2Port::Textures {

inline constexpr u32 Capacity = 1u << 15;

/// Installs the hooks on the GNF texture init and on the texture destroy.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The descriptor of texture, the game's texture object, when it is a 2D texture and ready, else
/// 0. Any thread; notes that a frame reads the texture.
u32 Get(VAddr texture);

/// As Get, for a cube map.
u32 GetCube(VAddr texture);

/// Any thread: asks the cache for what Get reads first of each of the count texture objects whose
/// addresses are at objects. An address may be null.
void Prefetch(const VAddr* objects, u32 count);

/// Any thread, once what Prefetch asked for has come: asks for what Get reads next of each.
void PrefetchRecords(const VAddr* objects, u32 count);

/// Draw thread: the descriptor of the 2D texture a T# of 8 words describes, when it is ready, else
/// 0. A T# of no texture object the hooks saw gets a texture of its own, converted like the others;
/// dynamic says whether it is linear, as the textures the CPU writes each frame are, and needs
/// Refresh. Never waits.
u32 GetBySharp(const u32* sharp, bool& dynamic);

/// Render thread, after Update: copies the guest memory of the textures of these descriptors into
/// their images again.
void Refresh(vk::CommandBuffer cmd, u64 frame, std::span<const u32> descriptors);

/// Render thread: handle, one of the handles Start keeps out of use, samples view from now on.
/// view is in ShaderReadOnlyOptimal layout whenever a shader samples it.
void SetExternal(u32 handle, vk::ImageView view);

/// Render thread: creates the texture array, its sampler and the detile pipelines.
bool Start(const Device& device, Memory& memory);

/// Render thread, before the frame's draws: records into cmd the conversions of new textures and
/// the level changes streaming chose, and frees images once the frames that may sample them are
/// done. Frames up to completed are done on the GPU. Never waits: a worker thread copies the
/// textures' data.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed);

/// The set with the texture array at binding 0, a repeating sampler at binding 1 and a clamping one
/// at binding 2, for fragment shaders.
vk::DescriptorSetLayout SetLayout();
vk::DescriptorSet Set();

struct Stats {
    u32 textures;
    u32 pending;
    /// Textures not converted: not a single 2D image or cube map, a tiling or format the renderer
    /// does not convert, a layout larger than the GNF data, or no memory.
    u32 not_2d;
    u32 unsupported;
    u32 too_large;
    u32 failed;
    /// Textures initialised again at the address of one never destroyed.
    u32 missed_destroys;
    /// Bytes converted, of the textures' whole data.
    u64 uploaded_bytes;
    u64 uploaded_data_bytes;
    u64 resident_bytes;
    /// Streaming: the budget, the bytes of every image held (replaced and new ones included), the
    /// quality (0 for every level of every texture), the textures read in the last two seconds
    /// and their full size, the full size of all textures, and the textures with fewer levels.
    u64 limit_bytes;
    u64 allocated_bytes;
    u32 quality;
    u32 used_textures;
    u64 used_bytes;
    u64 full_bytes;
    u32 reduced;
    /// Images made smaller and larger since the last TakeStats, and the bytes uploaded to grow.
    u32 shrinks;
    u32 grows;
    u64 grow_bytes;
};
Stats TakeStats();

/// Render thread: whether the textures in use have fewer levels than they hold, for the memory
/// budget's sake.
bool Squeezed();

} // namespace Gr2Port::Textures
