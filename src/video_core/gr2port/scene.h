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
/// Character draws with decals per packet.
inline constexpr u32 MaxDecals = 1024;
/// World draws with a material per packet, and the spheres that shade the world's ambient light.
inline constexpr u32 MaxMaterials = 8192;
/// Character draws per packet.
inline constexpr u32 MaxCharacters = 1024;
inline constexpr u32 MaxOccluders = 64;
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
    /// Draws with decals, and draws past MaxDecals, which lose theirs.
    u32 decals;
    u32 dropped_decals;
    /// Draws with a material, and draws past MaxMaterials, which lose theirs.
    u32 materials;
    u32 dropped_materials;
    /// Character draws, and those past MaxCharacters, which are not drawn.
    u32 characters;
    u32 dropped_characters;
    /// The draws are in three runs: world draws without alpha test from index 0, forward draws
    /// after them, and alpha-tested world draws from the end of the array down.
    u32 world_draws;
    u32 alpha_draws;
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
    /// The handle of the sky's first cube map, 0 when no sky is drawn, and the colour the scene
    /// starts from, which shows where no sky is drawn.
    u32 sky;
    float background[3];
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
    /// The index of the draw's decals in the packet, plus 1; 0 for none.
    u32 decal;
    /// The elements of the packed normal, the tangent and the vertex colour, encoded as position;
    /// format 0 for none.
    u32 normal;
    u32 tangent;
    u32 vertex_color;
    /// The index of the draw's material or, for a character's draw, its Character in the packet,
    /// plus 1; 0 for none.
    u32 material;
};
static_assert(sizeof(Draw) == 224);

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

/// A character draw's decals, as the game's CH_Main programs read them. Each of the three projects
/// the texture along its target direction onto the mesh around its position, in model space; a
/// vertex takes the nearest. The result multiplies the colour map.
struct Decal {
    float positions[3][4];
    float targets[3][4];
    float scales[3][2];
    /// The texture's handle.
    u32 texture;
    /// 1 when the colour map's alpha masks the decal.
    u32 color_alpha;
    /// The game's GradationColor: its alpha replaces the decal's colour with its colour.
    float gradation[4];
};
static_assert(sizeof(Decal) == 144);

/// What a world draw's material reads, from the game's G2_BG_Main constants (the pixel program's,
/// then the gradation of the vertex program's) and the item's texture objects.
struct Material {
    float color_scale[4];
    float layered_color_scale[4];
    float color_transform[4];
    float layered_color_transform[4];
    float normal_transform[4];
    float layered_normal_transform[4];
    float mask_transform[4];
    float punch_through;
    float roughness;
    float metallic;
    float translucency;
    float emissive;
    float parallax;
    float layered_roughness;
    float layered_metallic;
    float layered_translucency;
    float layered_emissive;
    float layered_parallax;
    float hatching_repeat;
    /// The colours a model's height blends between: the height where low applies in w of low, and
    /// the inverse of the distance to high in w of high.
    float gradation_low[4];
    float gradation_high[4];
    /// Texture handles; 0 is white. A normal map of 0 means the draw has none.
    u32 normal_map;
    u32 layered_color;
    u32 layered_normal;
    u32 mask;
    u32 hatching;
    u32 flags;
    u32 padding[2];
};
static_assert(sizeof(Material) == 224);
/// The material has a second layer, weighted by the vertex colour's alpha.
inline constexpr u32 MaterialLayered = 1;
/// The material's gradation applies.
inline constexpr u32 MaterialGradation = 2;

/// What a character draw reads: the game's G2_CH_Main pixel constants, then the normal bend of its
/// vertex constants and the item's texture objects. shaders/scene.glsl names the constants.
struct Character {
    u32 pixel_constants[152];
    float bend_position[4];
    float bend_range;
    float bend_bias;
    /// Texture handles; 0 is white. A normal map of 0 means the draw has none.
    u32 normal_map;
    u32 reflection;
    u32 detail;
    u32 padding[3];
};
static_assert(sizeof(Character) == 656);

/// The frame's sky, as the game's sky pixel programs read it. Rays run in the sky's space, which
/// the view's sky rotation sets. Matrices are as in Draw.
struct Sky {
    /// Clip space to the sky's space, without the camera's position.
    float inverse_view_projection[16];
    /// A ray to the direction each cube map is sampled in.
    float transforms[2][16];
    float sun_color[4];
    float sun_direction[4];
    /// The clouds' colour; w is their opacity.
    float cloud_color[4];
    /// Each cloud layer: its scale, its offset in xy after the scale, its texture coordinate scale.
    float cloud_layers[2][4];
    /// The clouds fade out from a scaled distance x over the inverse of y.
    float cloud_fade[2];
    /// The sun's angle in radians, below 0 for no disc, and the inverse of its soft edge.
    float sun_size;
    float sun_outer;
    /// The second cube map's share of the sky.
    float blend;
    /// The cube maps' handles: the first is never 0, the second is 0 when blend is 0.
    u32 cubes[2];
    /// The cloud texture's handle; 0 is white.
    u32 cloud;
};
static_assert(sizeof(Sky) == 304);

/// How the scene's HDR colour becomes the picture, as the game's exposure, Tonemap and Composite
/// programs read it.
struct Grade {
    /// The base exposure in stops, how many stops the meter may move it, the share of the way to
    /// the measured brightness each frame moves, and the brightness the meter aims for.
    float exposure;
    float adaption_range;
    float adaption_speed;
    float key_value;
    /// Each channel's curve: 7 knots evenly over 0 to 1.
    float knots[3][8];
    /// The lens: x and y shape it, z bends it.
    float distortion[4];
    /// The screen's wobble: its phase and its amount.
    float fluctuation[2];
    float saturation;
    /// 1 when the lens and the wobble apply.
    u32 distorted;
};
static_assert(sizeof(Grade) == 144);

/// The key light, ambient light and comic shading of the world, as the game's LightPass, IBL and
/// Resolve programs read them. Matrices are as in Draw.
struct Light {
    /// View space to each cascade's texture coordinates and depth, and back.
    float shadow_projection[Cascades][16];
    float inverse_shadow_projection[Cascades][16];
    /// View space to the sky's space.
    float view_to_sky[16];
    /// A depth d from 0 to 1 gives the distance 1 / (z (2d - 1) + w), and a pixel's view position
    /// from x and y.
    float inverse_projection[4];
    /// The first cascade ends at the distance y, the second at z.
    float split_depths[4];
    float light_color[4];
    /// The direction the key light's light travels in, in view space.
    float light_direction[4];
    /// The direction the occluders shade the ambient light along, in view space.
    float occluder_direction[4];
    /// The colours distant surfaces fade to, by the height of the view ray.
    float gradation_high[4];
    float gradation_middle[4];
    float gradation_low[4];
    float contour_color[4];
    /// Between these distances surfaces lose their highlights.
    float specular_fade[2];
    float shadow_fade_start;
    float ibl_intensity;
    float contour_blend;
    float contour_distant_blend;
    float contour_start;
    float contour_range;
    float gradation_start;
    float gradation_range;
    float background_start;
    float background_range;
    float background_intensity;
    float background_lod;
    float hatching_intensity;
    float toning;
    float occluder_intensity;
    float occluder_penumbra;
    float occluder_range;
    u32 occluders;
    /// The ambient light's cube map handles; 0 for none. The second pair applies by ibl_blend.
    u32 radiance[2];
    u32 irradiance[2];
    float ibl_blend;
    u32 padding[3];
    /// Occluders in view space: centre, radius.
    float occluder_spheres[MaxOccluders][4];
    /// The ambient light's spherical harmonics for characters: Ar, Ag, Ab, Br, Bg, Bb, Cc.
    float sh[7][4];
};
static_assert(sizeof(Light) % 16 == 0);

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
inline constexpr u64 DecalsOffset = SoftBodyJobsOffset + MaxSoftBodyJobs * sizeof(SoftBodyJob);
inline constexpr u64 SkyOffset = DecalsOffset + MaxDecals * sizeof(Decal);
inline constexpr u64 GradeOffset = SkyOffset + sizeof(Sky);
inline constexpr u64 LightOffset = GradeOffset + sizeof(Grade);
inline constexpr u64 MaterialsOffset = LightOffset + sizeof(Light);
inline constexpr u64 CharactersOffset = MaterialsOffset + MaxMaterials * sizeof(Material);
inline constexpr u64 PacketBytes = CharactersOffset + MaxCharacters * sizeof(Character);
static_assert(sizeof(Header) <= ShadowsOffset && ShadowsOffset + sizeof(Shadows) <= DrawsOffset &&
              DrawsOffset % 16 == 0 && CastersOffset % 16 == 0 && PalettesOffset % 16 == 0 &&
              SkinJobsOffset % 16 == 0 && EffectorsOffset % 16 == 0 &&
              SoftBodyJobsOffset % 16 == 0 && DecalsOffset % 16 == 0 && SkyOffset % 16 == 0 &&
              GradeOffset % 16 == 0 && LightOffset % 16 == 0 && MaterialsOffset % 16 == 0 &&
              CharactersOffset % 16 == 0);

} // namespace Gr2Port::Scene
