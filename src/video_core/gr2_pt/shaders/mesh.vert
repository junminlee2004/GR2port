// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(location = 0) out vec3 view_position;
layout(location = 1) flat out uint mesh_id;

// Format indices of the game's vertex format table.
const uint FormatRgba16Float = 7u;

vec3 LoadPosition(uint64_t vertices, uint stride, uint position) {
    const uint offset = position & 0xffu;
    const uint format = position >> 8;
    Words words = Words(vertices + uint64_t(gl_VertexIndex) * stride + offset);
    if (format == FormatRgba16Float) {
        return vec3(unpackHalf2x16(words.words[0]), unpackHalf2x16(words.words[1]).x);
    }
    return uintBitsToFloat(uvec3(words.words[0], words.words[1], words.words[2]));
}

void main() {
    Draws draws = scene_draws;
    const uint64_t vertices = draws.draws[gl_DrawID].vertices;
    const vec4 position = vec4(LoadPosition(vertices, draws.draws[gl_DrawID].stride,
                                            draws.draws[gl_DrawID].position), 1.0);
    view_position = (draws.draws[gl_DrawID].world_view * position).xyz;
    vec4 clip = draws.draws[gl_DrawID].world_view_projection * position;
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
    mesh_id = uint(vertices >> 8);
}
