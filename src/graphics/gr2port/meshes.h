// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <limits>

#include "graphics/gr2port/device.h"

namespace Xbyak {
class CodeGenerator;
}

namespace Gr2Port {
class Memory;
}

// The game's mesh shapes (SObj kind 6, built by the .gfx builder 0x1447990) copied into the
// renderer's memory: vertex data in a buffer per shape, indices in one shared index buffer. Each
// shape holds its handle at +0x184, which no game code reads or writes.
namespace Gr2Port::Meshes {

inline constexpr u32 Capacity = 1u << 16;

/// The indices of a chunk of a mesh's index data: whole triangles, from a multiple of 3 on.
inline constexpr u32 ChunkIndices = 384;

/// The box around the positions that a run of a mesh's chunks names.
struct ChunkBox {
    float low[3];
    float high[3];
};

// A record starts on a cache line: what a draw reads of it is in its first two.
struct alignas(64) Mesh {
    std::atomic<VAddr> shape{0};
    std::atomic<bool> ready{false};
    /// The collector's state: kept, evicting (no new draws, buffers live), evicted (buffers
    /// freed) or wanted again while evicting. Draws that ask for it set used to the second.
    std::atomic<u8> eviction{0};
    std::atomic<u32> used{0};
    /// The device address of the vertex data.
    u64 vertices = 0;
    /// The first index of the shape's index data in the shared index buffer, and its count.
    u32 first_index = 0;
    u32 indices = 0;
    u32 stride = 0;
    /// The bytes of vertex data, without the skin stream.
    u32 vertex_bytes = 0;
    /// The position element: byte offset in the low 8 bits, format index above.
    u32 position = 0;
    /// The texture coordinate element, encoded as position; format 0 when the shape has none.
    u32 texcoord = 0;
    /// The two-component element of the next semantic, encoded as position; format 0 for none.
    u32 texcoord1 = 0;
    /// The elements of the packed normal, the RGBA16 FLOAT tangent and the vertex colour, encoded
    /// as position; format 0 when the shape has none.
    u32 normal = 0;
    u32 tangent = 0;
    u32 vertex_color = 0;
    /// The box around the positions the shaders load from the vertex data, in the shape's own
    /// space. A shape whose positions the box cannot hold has one without bounds.
    float box_low[3]{-std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()};
    float box_high[3]{std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
    /// The boxes of the mesh's chunks as a tree, and the chunks; none for a mesh without a box or
    /// with one chunk. The node of chunks low to high comes first. Over more than one chunk, the
    /// nodes of its first half, up to middle = (low + high) / 2, follow it, and those of its
    /// second half start 2 * (middle - low) nodes after it.
    const ChunkBox* chunk_tree = nullptr;
    u32 chunks = 0;
    /// The device address of the skin stream (four RGBA16 FLOAT weights, then four RGBA16 SINT
    /// bone indices, 16 bytes a vertex), 0 when the shape is not skinned.
    u64 skin = 0;
    /// The greatest sum of a vertex's four skin weights. Infinity when the shape is not skinned,
    /// or has a weight that is below 0 or not a number.
    double weight_sum = std::numeric_limits<double>::infinity();
    /// Shapes with cloth: the device addresses of the SoftBody particle records (8 bytes each),
    /// constraints (4 bytes each) and vertex to particle map (a u16 per vertex), and the particle
    /// count; 0 for other shapes.
    u64 particles = 0;
    u64 constraints = 0;
    u64 particle_map = 0;
    u32 particle_count = 0;
};

/// Installs the hooks on the builder and on the model unload.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

/// The ready mesh of shape, or null. Any thread.
const Mesh* Get(VAddr shape);

/// Any thread: asks the cache for what Get(shape) and a draw read of the mesh. Reads the shape's
/// handle.
void Prefetch(VAddr shape);

/// Render thread: creates the shared index buffer.
bool Start(const Device& device, Memory& memory);

/// Render thread, before the frame's draws: records the copies of up to a frame's budget of new
/// shapes into cmd, and frees the meshes of unloaded models once the frames that may draw them
/// are done. Frames up to completed are done on the GPU.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed);

/// The buffer holding every mesh's 16-bit indices.
vk::Buffer IndexBuffer();

/// Render thread: records a copy of a ready mesh's vertex data, without the skin stream, to
/// destination at offset.
void CopyVertices(vk::CommandBuffer cmd, const Mesh& mesh, vk::Buffer destination, u64 offset);

struct Stats {
    u32 meshes;
    /// Of those, skinned meshes and meshes with cloth.
    u32 skinned;
    u32 cloth;
    /// Shapes whose cloth data names particles, vertices or constraints it does not have.
    u32 bad_cloth;
    u32 pending;
    /// Shapes the shaders cannot draw, or with no room.
    u32 skipped;
    /// Of those, shapes with indices past their vertex data.
    u32 bad_indices;
    u32 uploads_logged;
    /// Shapes built again at the address of a shape that was never unloaded.
    u32 missed_unloads;
    /// Unloaded shapes whose handle field does not hold their handle.
    u32 lost_handles;
    /// Shapes with no texture coordinates the shaders decode.
    u32 no_texcoord;
    u32 no_normal;
    u64 uploaded_bytes;
    u64 resident_bytes;
};
Stats TakeStats();

} // namespace Gr2Port::Meshes
