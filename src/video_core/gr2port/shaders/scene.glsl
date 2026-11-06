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
};

// Material flags.
const uint MaterialLayered = 1u;
const uint MaterialGradation = 2u;

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
};

// The meter's state, which lasts from frame to frame: the frame's sum of log luminance in units of
// 1 / MeterScale, the exposure the picture uses, and the brightness the meter has adapted to.
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Meter {
    int log_sum;
    float exposure;
    float adapted;
};

const float MeterScale = 256.0;

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Light {
    mat4 shadow_projection[Cascades];
    mat4 inverse_shadow_projection[Cascades];
    mat4 view_to_sky;
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
    uint padding[3];
    vec4 occluder_spheres[MaxOccluders];
};
