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
/// Character draws per packet, and their outlines.
inline constexpr u32 MaxCharacters = 1024;
inline constexpr u32 MaxContours = 512;
inline constexpr u32 MaxOccluders = 64;
/// Leaf draws and leaf shadow draws per packet, together.
inline constexpr u32 MaxLeaves = 2048;
/// World draws of four mixed layers per packet.
inline constexpr u32 MaxLayerMaterials = 1024;
/// Grass instances per packet, and the blades of all of them, the limit of the game's kernel.
inline constexpr u32 MaxGrass = 256;
inline constexpr u32 MaxBlades = 131072;
/// A blade in the renderer's buffer: root, fade, tip, instance. See shaders/scene.glsl.
inline constexpr u32 BladeBytes = 32;
/// Decals of the world per packet, the game's limit.
inline constexpr u32 MaxWorldDecals = 1024;
/// SelfTranslucent's stencil values, one draw list and one particle pass each.
inline constexpr u32 SelfTranslucentLists = 3;
/// Particle draws per packet, and what the game's simulation takes.
inline constexpr u32 MaxParticleDraws = 1024;
/// Meshes that place particles on themselves per packet, and the bytes of their bone palettes.
inline constexpr u32 MaxParticleSamples = 64;
inline constexpr u64 MaxParticleSamplePaletteBytes = 256 * 1024;
inline constexpr u64 ParticleEffectBytes = 0xc0;
inline constexpr u64 ParticleObjectBytes = 0x80;
inline constexpr u64 ParticleBytes = 0x40;
inline constexpr u64 MaxParticleEffectBytes = 0x600 * ParticleEffectBytes;
inline constexpr u32 MaxParticleOwners = 0x18000;
/// Descriptions the game rewrites each frame, at most, and the bytes of one.
inline constexpr u32 MaxParticleLiveDescriptions = 0x40;
inline constexpr u64 ParticleDescriptionBytes = 0x100;
/// The words of the move program's constants, and which one holds the pool's size.
inline constexpr u32 ParticleSceneWords = 72;
inline constexpr u32 ParticlePoolWord = 46;
/// A draw's first_instance holds its index above this bit, and the game's instance below it.
inline constexpr u32 ParticleDrawShift = 20;
/// G2_EF_Main draws per packet, and the game's sorted lists they come from.
inline constexpr u32 MaxEffectDraws = 256;
inline constexpr u32 EffectLists = 7;
/// G2_BG_Water draws per packet.
inline constexpr u32 MaxWaterDraws = 32;
/// G2_BG_Metaball draws per packet.
inline constexpr u32 MaxMetaballs = 256;
/// Textures of the world's draws that change each frame, per packet.
inline constexpr u32 MaxDynamicTextures = 16;
/// The game's limit on a view's point lights.
inline constexpr u32 MaxPointLights = 1024;
/// LightCompute's grid of the view: 60 columns, 36 rows and 8 depth slices, 48 light indices to a
/// cell, the last after the cell's lights being ~0.
inline constexpr u32 LightGridColumns = 60;
inline constexpr u32 LightGridRows = 36;
inline constexpr u32 LightGridSlices = 8;
inline constexpr u32 LightsPerCell = 48;
/// Materials of G2_BG_Glass draws per packet.
inline constexpr u32 MaxGlassMaterials = 256;
/// Branch instances the kernel sways per packet, and branch ribbon draws.
inline constexpr u32 MaxBranchJobs = 512;
inline constexpr u32 MaxBranches = 512;
/// The effector grid: 6 x 6 x 10 cells of the view, each listing up to 127 spheres and an end
/// marker.
inline constexpr u32 GridCells = 360;
inline constexpr u32 GridEntries = 128;
/// The screen drawing per packet: draws, runs, the game's targets, the textures it reads that
/// change each frame, and the bytes of the game's buffers it copies.
inline constexpr u32 MaxUiDraws = 2048;
inline constexpr u32 MaxUiRuns = 1024;
inline constexpr u32 MaxUiTargets = 32;
inline constexpr u32 MaxUiDynamicTextures = 64;
inline constexpr u64 MaxUiDataBytes = 4_MB;
/// The inputs of the screen drawing's vertex programs, at most.
inline constexpr u32 UiAttributes = 6;
/// The texture handles of the targets: the last MaxUiTargets of the texture array.
inline constexpr u32 UiTargetHandles = (1u << 15) - MaxUiTargets;
/// The game's primitive types of the screen drawing: triangle lists and strips.
inline constexpr u32 UiPrimitiveList = 4;
inline constexpr u32 UiPrimitiveStrip = 6;

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

/// The command lists of a frame's draws.
enum class DrawList : u32 {
    /// World draws without alpha test, that cull back faces and that cull nothing.
    OpaqueCulled,
    Opaque,
    /// Alpha-tested world draws, that cull back faces and that cull nothing.
    AlphaCulled,
    Alpha,
    /// Character draws.
    Character,
    /// G2_CH_Interior draws. world_view holds their world matrix. material names a Character whose
    /// pixel_constants start with the game's 44 words, with their cube maps in normal_map and
    /// reflection and a bend_bias of 0.
    Interior,
    /// opaque/Lambert draws. Both matrices hold the game's WorldViewProj. material names a
    /// Character whose pixel_constants hold the game's 8 pixel constant words, then the three
    /// ShadowProj of its vertex constants.
    Lambert,
    /// opaque/G2_BG_Vision draws, the screens of televisions. texture is the screen and
    /// texcoord_transform flips v. material names a Character whose pixel_constants hold the
    /// game's 12 pixel constant words, with the damage map in normal_map and the overlay in
    /// reflection.
    Vision,
    /// The televisions' glow, category 25: Vision draws for the bloom's image, whose word 9 holds
    /// the game's blend constant.
    VisionGlow,
    /// G2_BG_Glass draws. material names a GlassMaterial, texture is the normal map.
    Glass,
    /// SelfTranslucent draws of the characters with stencil value 1, 2 and 3. world_view holds
    /// the game's WorldViewProj and color_scale its colour; the first word of
    /// world_view_projection is the distance to move the skin along its normal.
    SelfTranslucent1,
    SelfTranslucent2,
    SelfTranslucent3,
    Count,
};

/// The parts of the capture's time.
enum class CapturePart : u32 {
    /// The frame's constants: shadows, sky, exposure, light.
    Constants,
    Deformers,
    Casters,
    /// The world's draws and materials.
    World,
    /// The characters' draws and outlines, and copying the commands to the packet.
    Characters,
    Count,
};

/// The particle system's vertex programs that are ported, by what they draw.
enum class ParticleShape : u32 {
    Sprite,
    Ribbon,
    Model,
    Count,
};

/// The programs of particles in a cascade's map.
enum class ParticleShadowProgram : u32 {
    Sprite,
    SpriteSoft,
    Model,
    Count,
};

/// A run's mode when it draws into the world's surfaces, and when it draws into a cascade's map:
/// the second plus the cascade.
inline constexpr u32 ParticleSurfaceMode = 6;
inline constexpr u32 ParticleShadowMode = 32;
/// The vertex and pixel programs of mesh particles in the world's surfaces, by what the mesh has.
inline constexpr u32 ParticleSurfaceVariants = 4;

/// The pixel programs of the particle system that are ported. Sprites and ribbons share a set, and
/// models have their own, in this order.
enum class ParticleProgram : u32 {
    Plain,
    PlainSoft,
    Lit,
    LitSoft,
    Fog,
    FogSoft,
    FogLit,
    FogLitSoft,
    Distortion,
    DistortionSoft,
    Blur,
    BlurSoft,
    Count,
};

/// The numbers of a packet's particle blocks.
struct ParticleHeader {
    /// Guest memory of the effect library, which changes when generation does, and of the noise.
    u64 description_source;
    u64 material_source;
    u64 curve_source;
    u64 noise_source;
    /// The game's sizes of a particle and an object buffer.
    u64 particle_bytes;
    u64 object_bytes;
    u32 generation;
    u32 objects;
    u32 particles;
    u32 draws;
    u32 runs;
    /// Draws past MaxParticleDraws, and items of classes or modes not ported.
    u32 dropped;
    u32 unported;
    /// The simulation's input did not fit the packet.
    u32 refused;
    /// Effects the game has running, and the time this block took to capture.
    u32 live_effects;
    u32 capture_ns;
    /// Descriptions of the library this packet holds fresh copies of.
    u32 live_descriptions;
    /// The commands of the draws without indices and of the draws with them.
    u32 vertex_commands;
    u32 indexed_commands;
    /// The runs that draw into the cascades' maps, and into the world's surfaces.
    u32 shadow_runs;
    u32 surface_runs;
    /// The meshes that place particles on themselves, and those without room or data.
    u32 samples;
    u32 dropped_samples;
};

/// The game's programs of the screen drawing that are ported.
enum class UiVertexProgram : u32 {
    Pass,
    PassFlat,
    Master,
    TransformB,
    Transform,
    Skinned,
    SkinnedB,
    Count,
};
enum class UiPixelProgram : u32 {
    Color,
    Copy,
    Gamma,
    Threshold,
    Movie,
    Sprite,
    Frame,
    Masked,
    Blur,
    BlurHistory,
    ColorControl,
    Count,
};

/// The numbers of a packet's screen drawing.
struct UiHeader {
    /// 1 when the frame graph did not run: the packet holds the screen drawing alone.
    u32 only;
    u32 draws;
    u32 runs;
    u32 targets;
    /// Textures read that change each frame.
    u32 dynamic;
    /// The target of the frame's back buffer, and the target the world's picture goes to.
    u32 picture;
    u32 world;
    /// Draws without room, state or data, draws of programs not ported, and reads of textures
    /// the renderer does not have.
    u32 dropped;
    u32 unported;
    u32 unknown_textures;
};

struct EffectHeader {
    u32 draws;
    u32 runs;
    /// Entries of the game's lists not drawn, and those of categories not ported.
    u32 dropped;
    u32 unported;
    /// A bit per list that holds draws, and one per list with a draw that reads the scene's copy.
    u32 lists;
    u32 background;
    /// G2_BG_Water draws in the packet, and items without room or a mesh.
    u32 water_draws;
    u32 dropped_water;
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
    /// The capture's time on the game's draw thread, and its parts.
    u32 capture_ns;
    /// The time since the last frame's capture began.
    u32 frame_gap_ns;
    u32 capture_parts_ns[static_cast<u32>(CapturePart::Count)];
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
    /// Outlines, and those that cannot be drawn.
    u32 contours;
    u32 dropped_contours;
    /// The commands of each draw list, which follow each other in the packet.
    u32 draw_commands[static_cast<u32>(DrawList::Count)];
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
    /// BloomOn when the game runs bloom, with BloomFeedback when it blends the last frame's in.
    u32 bloom;
    /// FogShadow when the fog's sun shadow runs.
    u32 fog;
    /// Counts of the changes to the inputs of the stratus and region maps, and of their darkness,
    /// which the sun's direction changes as well.
    u32 fog_maps[2];
    u32 fog_darkness[2];
    /// The guest addresses of the fog's two noise volumes, Perlin then fbm: 64^3 bytes each, x
    /// fastest. 0 until the game has made them.
    u64 fog_noise[2];
    /// Leaf records written; the commands of the leaves and of each cascade's leaf shadows; leaf
    /// items not drawn (no mesh, texture or room).
    u32 leaves;
    u32 leaf_commands;
    u32 leaf_shadow_commands[Cascades];
    u32 dropped_leaves;
    /// LayerMaterial records written; past MaxLayerMaterials draws count in dropped_materials.
    u32 layer_materials;
    /// Grass records written, the workgroups in x of their kernel, and grass items not drawn (no
    /// record, mesh, texture or room).
    u32 grass;
    u32 grass_groups;
    u32 dropped_grass;
    /// BranchJob records written, the workgroups in x of their kernel, and Branch records not run
    /// (no mesh, copy or room); Branch records' ribbons written and ribbon items not drawn.
    u32 branch_jobs;
    u32 branch_groups;
    u32 dropped_branch_jobs;
    u32 branches;
    u32 dropped_branches;
    /// WorldDecal records written, and decals not drawn (no colour map).
    u32 world_decals;
    u32 dropped_world_decals;
    /// GlassMaterial records written, and glass items not drawn.
    u32 glass_materials;
    u32 dropped_glass;
    /// MetaballDraw records written, the commands of the unskinned and of the skinned ones, and
    /// metaball items not drawn.
    u32 metaballs;
    u32 metaball_commands[2];
    u32 dropped_metaballs;
    /// The view's point lights, as in Light.
    u32 point_lights;
    /// Textures of the world's draws that change each frame, which the renderer copies again
    /// before drawing.
    u32 dynamic_textures;
    u32 dynamic[MaxDynamicTextures];
    ParticleHeader particles;
    EffectHeader effects;
    UiHeader ui;
    /// The items of each category in the frame, at most 65535.
    u16 category_items[ItemCategories];
};
static_assert(sizeof(Header) <= 1024);
inline constexpr u32 BloomOn = 1;
inline constexpr u32 BloomFeedback = 2;
inline constexpr u32 FogShadow = 1;

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

/// A draw of a mesh, read by the vertex shader at gl_InstanceIndex. Matrices are the game's:
/// row-major, positions multiply from the left, OpenGL clip space.
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
    /// With MaterialLayers or MaterialGem, the index of the draw's LayerMaterial.
    u32 layer_material;
    u32 padding;
};
static_assert(sizeof(Material) == 224);
/// The material has a second layer, weighted by the vertex colour's alpha.
inline constexpr u32 MaterialLayered = 1;
/// The material's gradation applies.
inline constexpr u32 MaterialGradation = 2;
/// The draw's surface is its LayerMaterial; the material holds only its gradation and hatching.
inline constexpr u32 MaterialLayers = 4;
/// The draw is a gem's: its LayerMaterial holds the game's G2_BG_Gem pixel constants, its
/// colour map first of the colour maps and its normal map first of the normal maps.
inline constexpr u32 MaterialGem = 8;

/// A surface of four mixed layers: the game's G2_BG_Layer pixel constants, which shaders/scene.glsl
/// names, then the layers' texture handles; 0 is white. A normal map of 0 means the layer has none.
struct LayerMaterial {
    u32 pixel_constants[80];
    u32 color_maps[4];
    u32 normal_maps[4];
};
static_assert(sizeof(LayerMaterial) == 352);

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

/// A character's outline: the game's G2_CH_Contour constants of the vertex, geometry and pixel
/// programs, whose names are in shaders/scene.glsl, then its mesh. The elements are as in Draw.
struct Contour {
    float vertex_constants[40];
    float geometry_constants[8];
    float pixel_constants[12];
    u64 vertices;
    u32 stride;
    u32 position;
    u32 normal;
    u32 bones;
    u64 skin;
    u64 palette;
    u32 padding[2];
};
static_assert(sizeof(Contour) == 288);

/// A tree's leaves, from the game's Branch draws: each patch of three vertices is a quadratic
/// Bezier curve that grows isolines leaves, as the game's leaf hull, domain, geometry and pixel
/// programs build them. Shadow casters use world, the shape values, texture, alpha_reference and
/// the tessellation with their cascade's view-projection. Matrices are as in Draw.
struct Leaf {
    float world_view_projection[16];
    float world[16];
    float view_projection[16];
    /// The colour at the tip of a leaf and at its base.
    float tip_color[4];
    float root_color[4];
    /// The shape's vertices; the position element as in Draw.
    u64 vertices;
    u32 stride;
    u32 position;
    /// The leaf texture's handle.
    u32 texture;
    /// Segments along each leaf, and leaves on each patch.
    float tessellation;
    float isolines;
    /// How far a leaf turns from the branch (the cosine of the angle), how far its width twists
    /// along it, its width, how far it droops and its length.
    float direction;
    float twist;
    float width;
    float gravity;
    float leaf_length;
    float alpha_reference;
    float translucency;
    float roughness;
    float metallic;
    /// The screen door's threshold.
    float punch_through;
    u32 padding[3];
};
static_assert(sizeof(Leaf) == 304);

/// A decal of the world: a box whose back faces blend the decal into the surfaces inside it, as the
/// game's Decal pass does. Matrices are as in Draw.
struct WorldDecal {
    /// The box's space, x and y from -1 to 1 and z from 0 to 1, to view and clip space.
    float world_view[16];
    float world_view_projection[16];
    /// View space to the decal's: the box's, with x from 1 to 0 and y from 0 to -1.
    float view_to_decal[16];
    float color_scale[4];
    float normal_scale[4];
    /// Texture coordinates are xy + zw * the decal's xy.
    float color_transform[4];
    float normal_transform[4];
    /// The direction away from the box's z in view space, and the parallax depth.
    float parallax[4];
    float roughness;
    float metallic;
    float translucency;
    /// Texture handles. A normal map of 0 means the decal has none.
    u32 color_map;
    u32 normal_map;
    u32 padding[3];
};
static_assert(sizeof(WorldDecal) == 304);

/// The constants of a G2_BG_Glass pixel program, the game's 48 words: the view's inverse, the
/// cascades' split depths, the key light's colour and direction, the normal map's transform, the
/// reflectance, the translucency, the refraction ratios, the projection's scales, the roughness and
/// the normal scale. Matrices are as in Draw.
struct GlassMaterial {
    float constants[48];
};
static_assert(sizeof(GlassMaterial) == 192);

/// What the game's effect, water and model particle programs fetch and sample: a mesh, encoded as
/// Draw's, and the handles of its displacement map, its other maps and its reflection cube.
struct EffectMesh {
    u64 vertices;
    u64 skin;
    u64 palette;
    u32 stride;
    u32 position;
    u32 texcoord;
    u32 texcoord1;
    u32 normal;
    u32 tangent;
    u32 vertex_color;
    u32 displacement;
    u32 textures[5];
    u32 reflection;
};
static_assert(sizeof(EffectMesh) == 80);

/// A mesh that places particles on itself: the game's PfxBufferSampleMesh, its vertices and, when
/// skinned, its skin stream and where in the packet its palette lies.
struct ParticleSample {
    u32 constants[28];
    u64 vertices;
    u64 weights;
    u64 palette_offset;
};
static_assert(sizeof(ParticleSample) == 136);

/// A particle draw: the game's PfxBufferCommon, then the handles of its four textures. A model's
/// draw adds how many meshes its set has and which this is, and the mesh. A mesh particle in the
/// world's surfaces adds the constants of the game's programs, and the handles of its colour map,
/// normal map, mask, layered colour map and layered normal map.
struct ParticleDraw {
    float constants[88];
    u32 textures[4];
    float model_count[2];
    u32 padding[2];
    EffectMesh mesh;
    float vertex_constants[56];
    float pixel_constants[40];
    u32 maps[5];
    u32 padding2;
};
static_assert(sizeof(ParticleDraw) == 872);

/// Consecutive particle commands of one pipeline and state. The pipeline is the vertex program's
/// shape times the programs per shape, plus the pixel program.
struct ParticleRun {
    u32 pipeline;
    u32 mode;
    u32 first;
    u32 count;
    /// Whether the draws test and write depth, and the game's cull mode: 0 none, 1 front, 2 back.
    u32 tested;
    u32 write;
    u32 cull;
    /// The commands are Command, with indices, or VertexCommand.
    u32 indexed;
};

/// A G2_EF_Main draw: the constants of the game's programs, then its mesh.
struct EffectDraw {
    u32 vertex_constants[104];
    u32 pixel_constants[72];
    EffectMesh mesh;
};
static_assert(sizeof(EffectDraw) == 784);

/// A G2_BG_Water draw: the constants of the game's programs, then its mesh.
struct WaterDraw {
    u32 vertex_constants[152];
    u32 pixel_constants[128];
    EffectMesh mesh;
};
static_assert(sizeof(WaterDraw) == 1200);

/// A G2_BG_Metaball draw, as shaders/pfx/metaball.vert and metaball.frag read it: the game's
/// vertex constants, with the neighbours its draw writes at +0x110 filled in, its pixel constants,
/// then its mesh, whose displacement is the displacement map.
struct MetaballDraw {
    u32 vertex_constants[96];
    u32 pixel_constants[12];
    EffectMesh mesh;
};
static_assert(sizeof(MetaballDraw) == 512);

/// Consecutive effect draws of one list and one state.
struct EffectRun {
    u32 list;
    u32 blend;
    u32 depth_mode;
    u32 skinned;
    u32 first;
    u32 count;
};

/// A Branch instance the renderer's kernel sways as the game's does, in its copy of the vertices,
/// from its shape's. The kernel writes the state, a timer and a phase per segment. Matrices are as
/// in Draw.
struct BranchJob {
    float world[16];
    float world_inverse[16];
    float world_view_projection[16];
    u64 source;
    u64 destination;
    u64 state;
    u32 segments;
    /// The sway's place in its cycle.
    float cycle;
    /// Clip depth to the effector grid's depth.
    float cluster_scale;
    u32 padding[3];
};
static_assert(sizeof(BranchJob) == 240);

/// A branch ribbon: each three vertices are a quadratic Bezier curve, drawn as a ribbon of segments
/// that faces the eye, as the game's branch programs build it. Matrices are as in Draw.
struct Branch {
    float world_view_projection[16];
    /// In model space.
    float eye[4];
    /// The texture's offset in xy and its repeat in zw, across and along the ribbon.
    float uv_range[4];
    float tip_color[4];
    float root_color[4];
    /// The instance's copy of the vertices, or the shape's; RGB32 FLOAT positions at offset 0.
    u64 vertices;
    u32 stride;
    u32 segments;
    /// The texture's handle.
    u32 texture;
    float tip_width;
    float root_width;
    float alpha_reference;
    float translucency;
    float roughness;
    float metallic;
    /// The screen door's threshold.
    float punch_through;
};
static_assert(sizeof(Branch) == 176);

/// A grass instance, whose tufts the renderer's kernel grows into blades as the game's grass
/// kernels do, and which its draw shades as the game's grass programs do. Matrices are as in Draw.
struct Grass {
    float world[16];
    float world_inverse[16];
    float world_view[16];
    /// By the view's own clip matrix, as the kernels read it.
    float world_view_projection[16];
    /// A direction, and in w the instance's speed: blades lean around the axis across it.
    float extra_force[4];
    /// The device address of the tufts: 64 bytes each, in the shape's vertices.
    u64 tufts;
    u32 tuft_count;
    /// The eye's distance where blades start to thin and where they end.
    float clip_min;
    float clip_max;
    /// The game's blade shape: how far blades scatter from their tuft's direction, how far from
    /// its root, how they sink with that, and how far they may lean from up.
    float coherency;
    float range_min;
    float range_max;
    float height_offset;
    float offset_curve;
    float normal_lerp;
    float normal_limit;
    /// Clip depth to the effector grid's depth.
    float cluster_scale;
    /// The wind's place in its cycle.
    float phase;
    u32 padding[2];
    /// As the draw reads it.
    float draw_world_view_projection[16];
    /// In model space.
    float eye[4];
    float tip_color[4];
    float root_color[4];
    float width;
    float alpha_reference;
    float translucency;
    float roughness;
    float metallic;
    /// The screen door's threshold.
    float punch_through;
    float normal_scale;
    /// Texture handles. A normal map of 0 means the draw has none.
    u32 color_map;
    u32 normal_map;
    u32 draw_padding[3];
};
static_assert(sizeof(Grass) == 496);

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
    /// The bloom's bright pass: per channel, then for luma in w, the levels where it starts and
    /// where it is full.
    float bloom_low[4];
    float bloom_high[4];
    /// The bright pass's power, the power its depth term takes, and the depth term's near and far
    /// scales.
    float bloom_correction[4];
    /// The depth term's near fade (x to y) and far fade (z to w).
    float bloom_depths[4];
    /// The bloom's strength per channel after the tone curve; w is 1 when the game runs bloom.
    float bloom_strength[4];
    /// The depth term's range that picks the blur's width, and the share of the last frame's bloom
    /// that stays.
    float bloom_width[2];
    float bloom_feedback;
    float bloom_padding;
};
static_assert(sizeof(Grade) == 240);

/// The volumetric fog, as the game's FogCloud, FogDist, FogCast and FogRender passes read it.
/// Matrices are as in Draw.
struct Fog {
    /// View space to the fog's space, to world space, and to the sky's rotated world space.
    float view_to_fog[16];
    float view_to_world[16];
    float view_to_rotated[16];
    /// 1 / P00, 1 / P11, near - far, -near, with the fog's own far distance.
    float projection[4];
    /// Each cumulus layer: its base, the inverse of half its height, its density and its scale;
    /// then its offsets in x and z.
    float cumulus[3][4];
    float cumulus_offsets[3][4];
    /// The stratus, then the region map: the height of its centre, the inverse of half its
    /// height, its density and scale; then its offsets in x and z and its darkness.
    float layers[2][4];
    float layer_offsets[2][4];
    /// The sun's colour, w the least extinction; its direction in view space; the noise's offset
    /// and, in w, its frequency.
    float light_color[4];
    float light_direction[4];
    float noise[4];
    /// The Henyey-Greenstein terms of the air and the clouds, and the clouds' colour.
    float phase_air[4];
    float phase_cloud[4];
    float cloud_color[4];
    float depth_scale;
    float darkness_scale;
    float noise_scale;
    float noise_attenuation;
    /// How much the sun's shadow darkens the air and the clouds; the jitter's two clocks.
    float shadow_darkness;
    float cloud_shadow;
    float time;
    float shadow_time;
    /// The apply pass: depth to distance as 1 / (x (2z - 1) + y), near, 1 / (far - near); the
    /// colour the fog fades to, w its share; the eye in rotated world space.
    float apply_depth[4];
    float apply_color[4];
    float eye[4];
    /// The height fog: its top, density and longest path.
    float puddle_height;
    float puddle_density;
    float puddle_range;
    /// The handle of the cumulus texture the layers read.
    u32 cumulus_texture;
    /// The stratus, then the region bake: top and bottom humidity, humidity, the cumulus' share,
    /// edge range and edge power.
    float bakes[2][8];
    /// The handles of the cumulus textures the bakes read.
    u32 bake_cumulus[2];
    u32 padding[2];
    /// The sun's direction for the darkness bakes.
    float sun[4];
};
static_assert(sizeof(Fog) == 656);

/// A point light as the view lists it: its position in view space and its range, its colour, the
/// distance its falloff starts at, the falloff's power and the share the screen-space shadow takes.
struct PointLight {
    float position[3];
    float range;
    float color[3];
    float falloff_start;
    float falloff_power;
    float shadow;
    u32 padding[2];
};
static_assert(sizeof(PointLight) == 48);

/// The key light, ambient light and comic shading of the world, as the game's LightPass, IBL and
/// Resolve programs read them. Matrices are as in Draw.
struct Light {
    /// View space to each cascade's texture coordinates and depth, and back.
    float shadow_projection[Cascades][16];
    float inverse_shadow_projection[Cascades][16];
    /// View space to the sky's space, and to the clip space of the last frame.
    float view_to_sky[16];
    float view_to_previous[16];
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
    /// The distance the world's lines measure depth against.
    float depth_range;
    /// The strength of the ambient occlusion, and the value that picks its program: above 0.9 its
    /// samples skip pixels that are not the world's.
    float occlusion_intensity;
    float occlusion_select;
    /// LightCompute's constants: the projection's x and y scales, the distance its depth slices
    /// start from, and the view's point lights.
    float grid_scale[2];
    float grid_start;
    u32 point_lights;
    /// How much the point lights fade where the key light is shadowed or faces away.
    float shadow_influence;
    u32 padding[3];
    /// Occluders in view space: centre, radius.
    float occluder_spheres[MaxOccluders][4];
    /// The ambient light's spherical harmonics for characters: Ar, Ag, Ab, Br, Bg, Bb, Cc.
    float sh[7][4];
};
static_assert(sizeof(Light) % 16 == 0);

/// An input of a screen draw: its vertices' device address, stride and the V#'s last word, which
/// holds the format and the components' order.
struct UiAttribute {
    u64 address;
    u32 stride;
    u32 format;
};

/// A screen draw, as the generated programs in shaders/pfx/ui_*.vert and ui_*.frag read it: the
/// device addresses of its indices (0 for none) and constants, the handles of its textures, its
/// samplers (bit 0 clamps, bit 1 picks point filtering), its inputs, and the size and first of
/// its indices.
struct UiDraw {
    u64 indices;
    u64 vs_constants;
    u64 vs_palette;
    u64 ps_constants;
    u32 textures[2];
    u32 samplers[2];
    UiAttribute attributes[UiAttributes];
    u32 index_bits;
    u32 first_index;
    u32 padding[2];
};
static_assert(sizeof(UiDraw) == 160);

/// Consecutive screen draws of one target, pair of programs, blend, primitive type and viewport:
/// x, y, width and height, with a negative height where the game's y scale is.
struct UiRun {
    u32 target;
    u32 vertex;
    u32 pixel;
    /// CB_BLEND0_CONTROL.
    u32 blend;
    u32 topology;
    float viewport[4];
    u32 first;
    u32 count;
};

/// A target of the game's: its address, which names it, and its size.
struct UiTarget {
    u64 base;
    u32 width;
    u32 height;
};

/// The indirect command of a draw or a caster, which gives its index as the first instance.
struct Command {
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    s32 vertex_offset;
    u32 first_instance;
};
static_assert(sizeof(Command) == 20);

/// The indirect command of a draw without indices, which gives its index as the first instance.
struct VertexCommand {
    u32 vertex_count;
    u32 instance_count;
    u32 first_vertex;
    u32 first_instance;
};
static_assert(sizeof(VertexCommand) == 16);

/// The index of a draw list's first command in the packet's commands.
inline u32 FirstCommand(const Header& header, DrawList list) {
    u32 first = 0;
    for (u32 i = 0; i < static_cast<u32>(list); i++) {
        first += header.draw_commands[i];
    }
    return first;
}

inline constexpr u64 ShadowsOffset = 1024;
inline constexpr u64 DrawsOffset = 2048;
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
inline constexpr u64 FogOffset = LightOffset + sizeof(Light);
inline constexpr u64 MaterialsOffset = FogOffset + sizeof(Fog);
inline constexpr u64 CharactersOffset = MaterialsOffset + MaxMaterials * sizeof(Material);
inline constexpr u64 ContoursOffset = CharactersOffset + MaxCharacters * sizeof(Character);
inline constexpr u64 ContourCommandsOffset = ContoursOffset + MaxContours * sizeof(Contour);
inline constexpr u64 LeavesOffset = ContourCommandsOffset + MaxContours * sizeof(Command);
inline constexpr u64 LeafCommandsOffset = LeavesOffset + MaxLeaves * sizeof(Leaf);
/// MaxLeaves commands per cascade, at cascade * MaxLeaves.
inline constexpr u64 LeafShadowCommandsOffset =
    LeafCommandsOffset + MaxLeaves * sizeof(VertexCommand);
inline constexpr u64 LayerMaterialsOffset =
    LeafShadowCommandsOffset + Cascades * MaxLeaves * sizeof(VertexCommand);
inline constexpr u64 GrassOffset = LayerMaterialsOffset + MaxLayerMaterials * sizeof(LayerMaterial);
/// The one draw of all blades. The renderer's kernel counts its vertices.
inline constexpr u64 GrassCommandOffset = GrassOffset + MaxGrass * sizeof(Grass);
inline constexpr u64 BranchJobsOffset = GrassCommandOffset + 16;
inline constexpr u64 BranchesOffset = BranchJobsOffset + MaxBranchJobs * sizeof(BranchJob);
inline constexpr u64 BranchCommandsOffset = BranchesOffset + MaxBranches * sizeof(Branch);
inline constexpr u64 WorldDecalsOffset = BranchCommandsOffset + MaxBranches * sizeof(VertexCommand);
inline constexpr u64 GlassMaterialsOffset = WorldDecalsOffset + MaxWorldDecals * sizeof(WorldDecal);
inline constexpr u64 ParticleSceneOffset =
    GlassMaterialsOffset + MaxGlassMaterials * sizeof(GlassMaterial);
/// The light's constants; the zeros after them stand for an empty list of point lights.
inline constexpr u64 ParticleLightOffset = ParticleSceneOffset + ParticleSceneWords * 4 + 16 * 8;
/// The fog's constants, for the fog programs.
inline constexpr u64 ParticleFogOffset = ParticleLightOffset + 256;
inline constexpr u32 ParticleFogWords = 36;
/// ExtractEmitterColor's constants as the game's FUN_014a7350 sets them: the view's matrix and
/// its projection, each transposed, both twice, then the size of the albedo's quarter copy.
inline constexpr u64 ParticleColorSceneOffset = ParticleLightOffset + 512;
inline constexpr u32 ParticleColorSceneWords = 68;
inline constexpr u64 ParticleDrawsOffset = ParticleLightOffset + 1024;
inline constexpr u64 ParticleCommandsOffset =
    ParticleDrawsOffset + MaxParticleDraws * sizeof(ParticleDraw);
inline constexpr u64 ParticleIndexedCommandsOffset =
    ParticleCommandsOffset + MaxParticleDraws * sizeof(VertexCommand);
inline constexpr u64 ParticleRunsOffset =
    ParticleIndexedCommandsOffset + MaxParticleDraws * sizeof(Command);
inline constexpr u64 ParticleLiveIndicesOffset =
    ParticleRunsOffset + MaxParticleDraws * sizeof(ParticleRun);
inline constexpr u64 ParticleLiveOffset =
    ParticleLiveIndicesOffset + MaxParticleLiveDescriptions * sizeof(u32);
inline constexpr u64 ParticleOwnersOffset =
    ParticleLiveOffset + MaxParticleLiveDescriptions * ParticleDescriptionBytes;
inline constexpr u64 ParticleEffectsOffset = ParticleOwnersOffset + MaxParticleOwners * sizeof(u64);
inline constexpr u64 ParticleSamplesOffset = ParticleEffectsOffset + MaxParticleEffectBytes;
inline constexpr u64 ParticleSamplePalettesOffset =
    ParticleSamplesOffset + MaxParticleSamples * sizeof(ParticleSample);
inline constexpr u64 EffectDrawsOffset =
    ParticleSamplePalettesOffset + MaxParticleSamplePaletteBytes;
inline constexpr u64 EffectCommandsOffset = EffectDrawsOffset + MaxEffectDraws * sizeof(EffectDraw);
inline constexpr u64 EffectRunsOffset = EffectCommandsOffset + MaxEffectDraws * sizeof(Command);
inline constexpr u64 WaterDrawsOffset = EffectRunsOffset + MaxEffectDraws * sizeof(EffectRun);
inline constexpr u64 WaterCommandsOffset = WaterDrawsOffset + MaxWaterDraws * sizeof(WaterDraw);
inline constexpr u64 MetaballDrawsOffset =
    (WaterCommandsOffset + MaxWaterDraws * sizeof(Command) + 15) & ~u64{15};
/// MaxMetaballs commands of the unskinned draws, then of the skinned ones.
inline constexpr u64 MetaballCommandsOffset =
    MetaballDrawsOffset + MaxMetaballs * sizeof(MetaballDraw);
/// The view's point lights, Light::point_lights of them.
inline constexpr u64 PointLightsOffset =
    (MetaballCommandsOffset + 2 * MaxMetaballs * sizeof(Command) + 15) & ~u64{15};
inline constexpr u64 UiTargetsOffset = PointLightsOffset + MaxPointLights * sizeof(PointLight);
inline constexpr u64 UiRunsOffset = UiTargetsOffset + MaxUiTargets * sizeof(UiTarget);
inline constexpr u64 UiDrawsOffset = UiRunsOffset + MaxUiRuns * sizeof(UiRun);
inline constexpr u64 UiCommandsOffset = UiDrawsOffset + MaxUiDraws * sizeof(UiDraw);
/// The handles of the textures that change each frame.
inline constexpr u64 UiDynamicOffset = UiCommandsOffset + MaxUiDraws * sizeof(VertexCommand);
/// Copies of the game's vertices, indices and constants.
inline constexpr u64 UiDataOffset = UiDynamicOffset + MaxUiDynamicTextures * sizeof(u32);
inline constexpr u64 PacketBytes = UiDataOffset + MaxUiDataBytes;
static_assert(sizeof(Header) <= ShadowsOffset && ShadowsOffset + sizeof(Shadows) <= DrawsOffset &&
              DrawsOffset % 16 == 0 && CastersOffset % 16 == 0 && PalettesOffset % 16 == 0 &&
              SkinJobsOffset % 16 == 0 && EffectorsOffset % 16 == 0 &&
              SoftBodyJobsOffset % 16 == 0 && DecalsOffset % 16 == 0 && SkyOffset % 16 == 0 &&
              GradeOffset % 16 == 0 && LightOffset % 16 == 0 && FogOffset % 16 == 0 &&
              MaterialsOffset % 16 == 0 && CharactersOffset % 16 == 0 && ContoursOffset % 16 == 0 &&
              LeavesOffset % 16 == 0 && LeafCommandsOffset % 16 == 0 &&
              LayerMaterialsOffset % 16 == 0 && GrassOffset % 16 == 0 &&
              GrassCommandOffset % 16 == 0 && BranchJobsOffset % 16 == 0 &&
              BranchesOffset % 16 == 0 && BranchCommandsOffset % 16 == 0 &&
              WorldDecalsOffset % 16 == 0 && GlassMaterialsOffset % 16 == 0 &&
              ParticleSceneOffset % 16 == 0 && ParticleDrawsOffset % 16 == 0 &&
              ParticleEffectsOffset % 16 == 0 && EffectDrawsOffset % 16 == 0 &&
              WaterDrawsOffset % 16 == 0 && MetaballDrawsOffset % 16 == 0 &&
              PointLightsOffset % 16 == 0 && UiDrawsOffset % 16 == 0 && UiDataOffset % 16 == 0);

} // namespace Gr2Port::Scene
