// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The scene packet, laid out as in video_core/gr2_pt/scene.h. Matrices are the game's row-major
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
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Draws {
    Draw draws[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words {
    uint words[];
};

// A bone palette: three rows of a 3x4 matrix per bone.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Rows {
    vec4 rows[];
};

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

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Shadows {
    mat4 inverse_view;
    mat4 view_projection[Cascades * CasterTables];
    vec4 light_direction;
    vec4 key_light_color;
    vec4 key_light_direction;
};
