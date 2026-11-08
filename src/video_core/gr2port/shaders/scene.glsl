// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The scene packet, laid out as in video_core/gr2port/scene.h. Matrices are the game's row-major
// ones: a column-major mat4 holds the rows as columns, so M * p is the game's p * M.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

struct Draw {
    mat4 world_view;
    mat4 world_view_projection;
    vec4 color_scale;
    vec4 texcoord_transform;
    uint64_t vertices;
    uint stride;
    uint position;
    uint texcoord;
    uint texture;
    uint64_t skin;
    uint64_t palette;
    uint bones;
    uint decal;
    uint normal;
    uint tangent;
    uint vertex_color;
    uint material;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Draws {
    Draw draws[];
};

struct Decal {
    vec4 positions[3];
    vec4 targets[3];
    vec2 scales[3];
    uint texture;
    uint color_alpha;
    vec4 gradation;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Decals {
    Decal decals[];
};

// The game's G2_BG_Main constants, then what its programs take from elsewhere.
struct Material {
    vec4 color_scale;
    vec4 layered_color_scale;
    vec4 color_transform;
    vec4 layered_color_transform;
    vec4 normal_transform;
    vec4 layered_normal_transform;
    vec4 mask_transform;
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
    vec4 gradation_low;
    vec4 gradation_high;
    uint normal_map;
    uint layered_color;
    uint layered_normal;
    uint mask;
    uint hatching;
    uint flags;
    uint layer_material;
};

// The game's G2_BG_Layer pixel constants, then the layers' texture handles.
struct LayerMaterial {
    vec4 color_transform[4];
    vec4 normal_transform[4];
    vec4 color_scale[4];
    float normal_scale[4];
    float roughness[4];
    float metallic[4];
    float translucency[4];
    float emissive[4];
    float parallax[4];
    uint blend[3];
    float hatching_repeat_u;
    float hatching_repeat_v;
    float punch_through;
    float contour_mask;
    float layer_padding;
    uint color_map[4];
    uint normal_map[4];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LayerMaterials {
    LayerMaterial layers[];
};

// The game's G2_BG_Gem pixel constants, then its maps, in the place of a LayerMaterial.
struct GemMaterial {
    vec4 color_scale;
    vec4 color;
    vec2 projection;
    vec2 offset;
    vec2 scale;
    float normal_scale;
    float roughness;
    float metallic;
    float translucency;
    float emissive;
    float refraction;
    float punch_through;
    float constants_padding[59];
    uint color_map;
    uint color_maps_padding[3];
    uint normal_map;
    uint normal_maps_padding[3];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GemMaterials {
    GemMaterial gems[];
};

// The game's G2_CH_Main pixel constants, then what its programs take from elsewhere.
struct Character {
    mat4 view_inverse;
    vec4 split_depths;
    vec4 highlight_color;
    vec4 diffuse_color;
    vec4 ambient_color;
    vec4 shadow_color;
    vec4 specular_color;
    vec4 color_scale;
    vec4 gradation_color;
    vec4 light_color;
    vec4 light_direction;
    vec4 camera_light_color;
    vec4 target_size_inverse;
    vec4 sh_unset[7];
    vec2 normal_repeat;
    vec2 normal_offset;
    vec2 reflection_offset;
    vec2 inverse_z;
    vec2 punch_through;
    uint point_lights;
    float highlight_border;
    float highlight_smooth;
    float highlight_fresnel;
    float diffuse_border;
    float diffuse_smooth;
    float diffuse_line;
    float shadow_border;
    float shadow_smooth;
    float shadow_fresnel;
    float shadow_gradation;
    float specular_border;
    float specular_smooth;
    float specular_fresnel;
    float rim_smooth;
    float rim_border;
    float mask_density;
    int reflection_mode;
    float reflection_blend;
    float reflection_repeat;
    float reflection_distortion;
    float detail_border;
    float detail_blend;
    float detail_repeat;
    float detail_mask;
    uint alpha_step;
    float alpha_reference;
    float light_intensity_scale;
    float shadow_influence;
    float camera_light_attenuation;
    float camera_light_inner;
    float camera_light_outer;
    float camera_light_directivity;
    float self_translucency;
    float self_translucency_start;
    float self_translucency_depth;
    float self_translucency_offset;
    float self_translucency_power;
    float self_translucency_scale;
    uint self_translucency_mode;
    uint bloom_fresnel_mode;
    float bloom_fresnel_border;
    float bloom_fresnel_smooth;
    float ibl_blend;
    float vertex_shadow;
    float rim_lighting;
    float rim_intensity;
    float padding[3];
    vec4 bend_position;
    float bend_range;
    float bend_bias;
    uint normal_map;
    uint reflection;
    uint detail;
    float glow;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Characters {
    Character characters[];
};

const uint CharacterBytes = 656u;

// The start of a G2_CH_Interior draw's Character: the game's pixel constants.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer InteriorConstants {
    mat4 rotation;
    vec4 view;
    vec4 center;
    vec4 reflectance;
    // xyz scale the cube map's colour by 1 + xyz in a band of its alpha around w.
    vec4 band[2];
    vec3 fresnel_floor;
    float roughness;
    float fresnel_bias;
    float fresnel_scale;
    float size_squared[2];
};

// The start of an opaque/Lambert draw's Character: the game's pixel constants, the key light's
// direction in the model's space and the colour scale, then the ShadowProj of its vertex constants.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LambertConstants {
    vec4 light_direction;
    vec4 color_scale;
    mat4 shadow_projection[3];
};

// The start of a G2_BG_Vision draw's Character: the game's pixel constants.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer VisionConstants {
    // The screen's texel width and width, the offset added to the picture, and a word unread.
    vec4 movie_size;
    // The overlay's coordinates: offset xy, then scale zw of the screen's.
    vec4 post_texcoord;
    float punch_through;
    // The scale of the picture: 1 on the screen, the blend constant in the glow.
    float glow;
};

// A character's outline: the game's G2_CH_Contour constants of each stage, then its mesh.
struct Contour {
    mat4 world_view;
    mat4 world_view_projection;
    vec4 fade_center;
    float transparency;
    float fading;
    vec2 padding;
    vec2 inverse_z;
    float size_amplitude;
    float fading_scale;
    float threshold;
    float offset;
    vec2 bias;
    vec4 color;
    float size;
    float line_amplitude;
    float line_cycle;
    float curve_amplitude;
    float curve_cycle;
    float softness;
    vec2 pixel_padding;
    uint64_t vertices;
    uint stride;
    uint position;
    uint normal;
    uint bones;
    uint64_t skin;
    uint64_t palette;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Contours {
    Contour contours[];
};

// A tree's leaves: the game's leaf constants of each stage, then the shape's vertices.
struct Leaf {
    mat4 world_view_projection;
    mat4 world;
    mat4 view_projection;
    vec4 tip_color;
    vec4 root_color;
    uint64_t vertices;
    uint stride;
    uint position;
    uint texture;
    float tessellation;
    float isolines;
    float direction;
    float twist;
    float width;
    float gravity;
    float leaf_length;
    float alpha_reference;
    float translucency;
    float roughness;
    float metallic;
    float punch_through;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Leaves {
    Leaf leaves[];
};

// The constants of a G2_BG_Glass pixel program.
struct GlassMaterial {
    mat4 view_inverse;
    vec4 split_depths;
    vec4 light_color;
    vec4 light_direction;
    vec4 normal_transform;
    vec4 reflectance;
    vec4 translucency;
    vec4 refraction;
    vec2 projection;
    float roughness;
    float normal_scale;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GlassMaterials {
    GlassMaterial materials[];
};

// A box of the world that blends a decal into the surfaces inside it.
struct WorldDecal {
    mat4 world_view;
    mat4 world_view_projection;
    mat4 view_to_decal;
    vec4 color_scale;
    vec4 normal_scale;
    vec4 color_transform;
    vec4 normal_transform;
    vec4 parallax;
    float roughness;
    float metallic;
    float translucency;
    uint color_map;
    uint normal_map;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer WorldDecals {
    WorldDecal decals[];
};

// A branch ribbon: the game's branch constants of each stage, then the instance's vertices.
struct Branch {
    mat4 world_view_projection;
    vec4 eye;
    vec4 uv_range;
    vec4 tip_color;
    vec4 root_color;
    uint64_t vertices;
    uint stride;
    uint segments;
    uint texture;
    float tip_width;
    float root_width;
    float alpha_reference;
    float translucency;
    float roughness;
    float metallic;
    float punch_through;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Branches {
    Branch branches[];
};

// A grass instance: what the game's grass kernel reads, then what its draw reads.
struct Grass {
    mat4 world;
    mat4 world_inverse;
    mat4 world_view;
    mat4 world_view_projection;
    vec4 extra_force;
    uint64_t tufts;
    uint tuft_count;
    float clip_min;
    float clip_max;
    float coherency;
    float range_min;
    float range_max;
    float height_offset;
    float offset_curve;
    float normal_lerp;
    float normal_limit;
    float cluster_scale;
    float phase;
    uint grass_padding[2];
    mat4 draw_world_view_projection;
    vec4 eye;
    vec4 tip_color;
    vec4 root_color;
    float width;
    float alpha_reference;
    float translucency;
    float roughness;
    float metallic;
    float punch_through;
    float normal_scale;
    uint color_map;
    uint normal_map;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer GrassInstances {
    Grass instances[];
};

// A blade of grass in its instance's model space: how far its root is inside the fade, and the
// instance's index.
struct Blade {
    vec3 root;
    float fade;
    vec3 tip;
    uint instance;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Blades {
    Blade blades[];
};

const uint MaxBlades = 131072u;

// Material flags.
const uint MaterialLayered = 1u;
const uint MaterialGradation = 2u;
const uint MaterialLayers = 4u;
const uint MaterialGem = 8u;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Materials {
    Material materials[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words {
    uint words[];
};

// A bone palette: three rows of a 3x4 matrix per bone.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Rows {
    vec4 rows[];
};

// The game's packed normal: three unsigned components in the exponent and upper mantissa bits of
// a half float, 11, 11 and 10 bits, holding n * 0.5 + 0.5.
vec3 UnpackNormal(uint word) {
    const vec3 halves = vec3(unpackHalf2x16((word << 4) & 0x7ff0u).x,
                             unpackHalf2x16((word >> 7) & 0x7ff0u).x,
                             unpackHalf2x16((word >> 17) & 0x7fe0u).x);
    return halves * 2.0 - 1.0;
}

// Row r of a bone. A bone past the palette reads zeros, as the game's buffer loads past its records
// do.
vec4 BoneRow(Rows palette, uint bones, int bone, uint r) {
    return uint(bone) < bones ? palette.rows[uint(bone) * 3u + r] : vec4(0.0);
}

struct Caster {
    mat4 world;
    uint64_t vertices;
    uint stride;
    uint position;
    uint table;
    uint texture;
    uint texcoord;
    float alpha_reference;
    uint64_t skin;
    uint64_t palette;
    uint bones;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Casters {
    Caster casters[];
};

const uint Cascades = 3u;
const uint CasterTables = 3u;
const uint MaxOccluders = 64u;
const uint LightGridColumns = 60u;
const uint LightGridRows = 36u;
const uint LightGridSlices = 8u;
const uint LightsPerCell = 48u;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Shadows {
    mat4 inverse_view;
    mat4 view_projection[Cascades * CasterTables];
    vec4 light_direction;
    vec4 key_light_color;
    vec4 key_light_direction;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Sky {
    mat4 inverse_view_projection;
    mat4 transforms[2];
    vec4 sun_color;
    vec4 sun_direction;
    vec4 cloud_color;
    vec4 cloud_layers[2];
    vec2 cloud_fade;
    float sun_size;
    float sun_outer;
    float blend;
    uint cubes[2];
    uint cloud;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Grade {
    float exposure;
    float adaption_range;
    float adaption_speed;
    float key_value;
    float knots[3][8];
    vec4 distortion;
    vec2 fluctuation;
    float saturation;
    uint distorted;
    vec4 bloom_low;
    vec4 bloom_high;
    vec4 bloom_correction;
    vec4 bloom_depths;
    vec4 bloom_strength;
    vec2 bloom_width;
    float bloom_feedback;
    float bloom_padding;
};

// The meter's state, which lasts from frame to frame: the frame's sum of log luminance in units of
// 1 / MeterScale, the exposure the picture uses, and the brightness the meter has adapted to.
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Meter {
    int log_sum;
    float exposure;
    float adapted;
};

const float MeterScale = 256.0;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Fog {
    mat4 view_to_fog;
    mat4 view_to_world;
    mat4 view_to_rotated;
    vec4 projection;
    vec4 cumulus[3];
    vec4 cumulus_offsets[3];
    vec4 layers[2];
    vec4 layer_offsets[2];
    vec4 light_color;
    vec4 light_direction;
    vec4 noise;
    vec4 phase_air;
    vec4 phase_cloud;
    vec4 cloud_color;
    float depth_scale;
    float darkness_scale;
    float noise_scale;
    float noise_attenuation;
    float shadow_darkness;
    float cloud_shadow;
    float time;
    float shadow_time;
    vec4 apply_depth;
    vec4 apply_color;
    vec4 eye;
    float puddle_height;
    float puddle_density;
    float puddle_range;
    uint cumulus_texture;
    vec4 bakes[2][2];
    uint bake_cumulus[2];
    uint fog_padding[2];
    vec4 sun;
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Light {
    mat4 shadow_projection[Cascades];
    mat4 inverse_shadow_projection[Cascades];
    mat4 view_to_sky;
    mat4 view_to_previous;
    vec4 inverse_projection;
    vec4 split_depths;
    vec4 light_color;
    vec4 light_direction;
    vec4 occluder_direction;
    vec4 gradation_high;
    vec4 gradation_middle;
    vec4 gradation_low;
    vec4 contour_color;
    vec2 specular_fade;
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
    uint occluders;
    uint radiance[2];
    uint irradiance[2];
    float ibl_blend;
    float depth_range;
    float occlusion_intensity;
    float occlusion_select;
    vec2 grid_scale;
    float grid_start;
    uint point_lights;
    float shadow_influence;
    uint shadow_trace;
    uint light_padding[2];
    vec4 occluder_spheres[MaxOccluders];
    vec4 sh[7];
};

struct PointLight {
    vec3 position;
    float range;
    vec3 color;
    float falloff_start;
    float falloff_power;
    float shadow;
    uint padding[2];
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer PointLights {
    PointLight point_lights[];
};

// LightCompute's grid: LightsPerCell light indices to a cell, the last after its lights ~0.
layout(buffer_reference, std430, buffer_reference_align = 4) buffer LightGrid {
    uint light_indices[];
};
