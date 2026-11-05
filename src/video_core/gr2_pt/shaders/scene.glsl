// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The scene packet's draws, laid out as in video_core/gr2_pt/scene.h. Matrices are the game's
// row-major ones: a column-major mat4 holds the rows as columns, so M * p is the game's p * M.

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
};

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Draws {
    Draw draws[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words {
    uint words[];
};

layout(push_constant) uniform Constants {
    Draws scene_draws;
};
