// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The layout of a frame's scene packet in the scene ring; shaders/scene.glsl mirrors it.
namespace Gr2Port::Scene {

inline constexpr u32 MaxDraws = 32768;
inline constexpr u32 MaxCasters = 4096;
inline constexpr u32 Cascades = 3;
/// The view-projections of each cascade: the plain one, and two that push casters along the light
/// (item flags 0x10 and 0x20).
inline constexpr u32 CasterTables = 3;
/// The command lists of each cascade: opaque casters that cull back faces and that cull nothing,
/// then alpha-tested casters in the same order.
inline constexpr u32 CasterLists = 4;
/// Bone palette rows per packet: each bone is three rows of a 3x4 matrix.
inline constexpr u32 MaxPaletteRows = 1u << 16;
/// The game's item categories.
inline constexpr u32 ItemCategories = 37;
/// Deformed instances the renderer skins per packet.
inline constexpr u32 MaxSkinJobs = 256;
/// Deformed instances the renderer simulates as cloth per packet, and the effector spheres they
/// collide with.
inline constexpr u32 MaxSoftBodyJobs = 512;
inline constexpr u32 MaxSpheres = 256;
/// The effector grid: 6 x 6 x 10 cells of the view, each listing up to 127 spheres and an end
/// marker.
inline constexpr u32 GridCells = 360;
inline constexpr u32 GridEntries = 128;

/// Why the capture does not run a SoftBody record.
enum class SoftBodyDrop : u32 {
    /// The shape's mesh is not uploaded.
    Mesh,
    /// The instance's copy is not ready.
    Copy,
    /// The record's fields are not the shape's cloth data.
    Record,
    /// The game's constants do not fit the mesh.
    Constants,
    /// The packet already holds MaxSoftBodyJobs jobs.
    JobCap,
    Count,
};

/// The start of each packet.
struct Header {
    u64 frame;
    /// The index count of all draws, and of the largest.
    u64 indices;
    u32 max_indices;
    u32 draws;
    /// Items with no drawable mesh or palette, or past MaxDraws.
    u32 dropped;
    /// The capture's time on the game's draw thread.
    u32 capture_ns;
    /// Draws whose colour texture is not converted.
    u32 untextured;
    /// Draws of skinned meshes; draws of items whose vertices the game's compute deforms each
    /// frame, from the renderer's copies; deformed items whose copy is not ready; palette rows
    /// written.
    u32 skinned;
    u32 deformed;
    u32 deformed_waiting;
    u32 palette_rows;
    /// Skin jobs written, the workgroups of the largest, and Skin records not written.
    u32 skin_jobs;
    u32 skin_groups;
    u32 dropped_skins;
    /// SoftBody jobs written, the workgroups of the largest particle count and of the largest
    /// vertex count, SoftBody records not written by reason, and the effector spheres copied.
    u32 soft_body_jobs;
    u32 soft_body_groups;
    u32 write_groups;
    u32 dropped_soft_bodies[static_cast<u32>(SoftBodyDrop::Count)];
    u32 spheres;
    /// Shadow casters written, of them skinned ones; casters with no drawable mesh or palette, or
    /// past MaxCasters.
    u32 casters;
    u32 skinned_casters;
    u32 dropped_casters;
    /// Casters drawn from deformed copies.
    u32 deformed_casters;
    /// The commands of each cascade's lists, and the indices of each cascade.
    u32 caster_commands[Cascades][CasterLists];
    u32 caster_indices[Cascades];
    /// One bit per cascade the game fills.
    u32 cascades;
    /// The game's depth bias of each cascade in GCN units: constant, then slope.
    float depth_bias[Cascades][2];
    /// The items of each category in the frame, at most 65535.
    u16 category_items[ItemCategories];
};
static_assert(sizeof(Header) <= 512);

/// The frame's shadow cascades. Matrices are as in Draw.
struct Shadows {
    /// View space to world space.
    float inverse_view[16];
    /// World space to each cascade's clip space, at cascade * CasterTables + table.
    float view_projection[Cascades * CasterTables][16];
    /// The direction toward the key light in view space, from the first cascade; w is 0.
    float light_direction[4];
    /// The game's key light: colour times intensity (w = intensity), and the view-space direction
    /// its light travels in.
    float key_light_color[4];
    float key_light_direction[4];
};
static_assert(sizeof(Shadows) == 688);

/// A draw of a mesh, read by the vertex shader at gl_DrawID. Matrices are the game's: row-major,
/// positions multiply from the left, OpenGL clip space.
struct Draw {
    float world_view[16];
    float world_view_projection[16];
    /// The material's colour scale; the alpha is unused.
    float color_scale[4];
    /// Texture coordinates are xy + zw * the vertex's.
    float texcoord_transform[4];
    u64 vertices;
    u32 stride;
    /// The position element: byte offset in the low 8 bits, format index above.
    u32 position;
    /// The texture coordinate element, encoded as position; format 0 when there is none.
    u32 texcoord;
    /// The colour texture's handle; 0 is white.
    u32 texture;
    /// Skinned meshes: the device addresses of the skin stream (four RGBA16 FLOAT weights, then
    /// four RGBA16 SINT bone indices, 16 bytes a vertex) and of the palette, and the palette's
    /// bone count. bones is 0 for meshes that are not skinned.
    u64 skin;
    u64 palette;
    u32 bones;
    u32 padding;
};
static_assert(sizeof(Draw) == 208);

/// A static shadow caster, read by the vertex shader at gl_InstanceIndex.
struct Caster {
    float world[16];
    u64 vertices;
    u32 stride;
    u32 position;
    /// Its view-projection in the cascade's tables.
    u32 table;
    /// Alpha-tested casters: the colour texture's handle, the texture coordinate element (as
    /// Draw's) and the alpha below which the caster lets light through.
    u32 texture;
    u32 texcoord;
    float alpha_reference;
    /// Skinned casters: as Draw's.
    u64 skin;
    u64 palette;
    u32 bones;
    u32 padding[3];
};
static_assert(sizeof(Caster) == 128);

/// A deformed instance the renderer skins as the game's Skin deformer does: from its shape's
/// vertex data at source into the renderer's copy at destination, both of stride bytes a vertex
/// with the position as RGB32 FLOAT at 0. The skin stream and palette are as Draw's.
struct SkinJob {
    u64 source;
    u64 destination;
    u64 skin;
    u64 palette;
    u32 bones;
    u32 vertices;
    u32 stride;
    /// The byte offsets of the packed normal and of the RGBA16 FLOAT tangent; 0 for none.
    u32 normal;
    u32 tangent;
    u32 padding[3];
};
static_assert(sizeof(SkinJob) == 64);

/// The effector constants as the game's effector program reads them, then the spheres: 32 bytes
/// each, {center xyz, radii (outer and inner half floats), ..., mask and strength half float}.
struct Effectors {
    /// World space to view space.
    float view[16];
    /// The depth the 10 slices cover, the depth where they start and the depth of one slice.
    float depth;
    float start;
    float slice;
    u32 count;
    float projection_x;
    float projection_y;
    u32 padding[2];
    u32 spheres[MaxSpheres][8];
};
static_assert(sizeof(Effectors) == 96 + MaxSpheres * 32);

/// A deformed instance the renderer simulates as the game's SoftBody deformer does. Anchors come
/// from skinned (the instance's copy after Skin, or its shape's vertex data); particles,
/// constraints and map are the shape's, state is the renderer's (see Deformers::Copy), and the
/// write pass puts positions into the copy at destination. constants holds the game's simulate
/// constants, or its reset constants when reset is 1; write holds its write constants.
struct SoftBodyJob {
    u64 skinned;
    u64 particles;
    u64 constraints;
    u64 map;
    u64 state;
    u64 destination;
    u32 reset;
    u32 padding[3];
    u32 constants[60];
    u32 write[20];
};
static_assert(sizeof(SoftBodyJob) == 384);

/// The indirect command of a draw, at the same index as the draw. A caster's command gives the
/// caster's index as its first instance.
struct Command {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    s32 vertex_offset;
    u32 first_instance;
};
static_assert(sizeof(Command) == 20);

inline constexpr u64 ShadowsOffset = 512;
inline constexpr u64 DrawsOffset = 1280;
inline constexpr u64 CommandsOffset = DrawsOffset + MaxDraws * sizeof(Draw);
inline constexpr u64 CastersOffset = CommandsOffset + MaxDraws * sizeof(Command);
/// MaxCasters commands per cascade and list, at (cascade * CasterLists + list) * MaxCasters.
inline constexpr u64 CasterCommandsOffset = CastersOffset + MaxCasters * sizeof(Caster);
/// MaxPaletteRows rows of 16 bytes.
inline constexpr u64 PalettesOffset =
    CasterCommandsOffset + Cascades * CasterLists * MaxCasters * sizeof(Command);
inline constexpr u64 SkinJobsOffset = PalettesOffset + MaxPaletteRows * 16;
inline constexpr u64 EffectorsOffset = SkinJobsOffset + MaxSkinJobs * sizeof(SkinJob);
inline constexpr u64 SoftBodyJobsOffset = EffectorsOffset + sizeof(Effectors);
inline constexpr u64 PacketBytes = SoftBodyJobsOffset + MaxSoftBodyJobs * sizeof(SoftBodyJob);
static_assert(sizeof(Header) <= ShadowsOffset && ShadowsOffset + sizeof(Shadows) <= DrawsOffset &&
              DrawsOffset % 16 == 0 && CastersOffset % 16 == 0 && PalettesOffset % 16 == 0 &&
              SkinJobsOffset % 16 == 0 && EffectorsOffset % 16 == 0 &&
              SoftBodyJobsOffset % 16 == 0);

} // namespace Gr2Port::Scene
