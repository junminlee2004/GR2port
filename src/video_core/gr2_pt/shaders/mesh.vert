// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(location = 0) out vec3 view_position;
layout(location = 1) out vec2 texcoord;
layout(location = 2) flat out vec3 color_scale;
layout(location = 3) flat out uint texture_index;

// Format indices of the game's vertex format table.
const uint FormatRg32Float = 2u;
const uint FormatRg16Float = 6u;
const uint FormatRgba16Float = 7u;
const uint FormatRg16Sint = 12u;

Words Element(uint64_t vertices, uint stride, uint element) {
    return Words(vertices + uint64_t(gl_VertexIndex) * stride + (element & 0xffu));
}

vec3 LoadPosition(uint64_t vertices, uint stride, uint position) {
    Words words = Element(vertices, stride, position);
    if ((position >> 8) == FormatRgba16Float) {
        return vec3(unpackHalf2x16(words.words[0]), unpackHalf2x16(words.words[1]).x);
    }
    return uintBitsToFloat(uvec3(words.words[0], words.words[1], words.words[2]));
}

vec2 LoadTexcoord(uint64_t vertices, uint stride, uint element) {
    const uint format = element >> 8;
    if (format == 0u) {
        return vec2(0.0);
    }
    Words words = Element(vertices, stride, element);
    if (format == FormatRg16Sint) {
        // The game's vertex shaders scale 16-bit integer texture coordinates by 1/1024.
        const uint word = words.words[0];
        return vec2(int(word << 16) >> 16, int(word) >> 16) / 1024.0;
    }
    if (format == FormatRg16Float) {
        return unpackHalf2x16(words.words[0]);
    }
    return uintBitsToFloat(uvec2(words.words[0], words.words[1]));
}

void main() {
    Draws draws = scene_draws;
    const uint64_t vertices = draws.draws[gl_DrawID].vertices;
    const uint stride = draws.draws[gl_DrawID].stride;
    const vec4 position =
        vec4(LoadPosition(vertices, stride, draws.draws[gl_DrawID].position), 1.0);
    view_position = (draws.draws[gl_DrawID].world_view * position).xyz;
    vec4 clip = draws.draws[gl_DrawID].world_view_projection * position;
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
    const vec4 transform = draws.draws[gl_DrawID].texcoord_transform;
    texcoord = transform.xy +
               transform.zw * LoadTexcoord(vertices, stride, draws.draws[gl_DrawID].texcoord);
    color_scale = draws.draws[gl_DrawID].color_scale.rgb;
    texture_index = draws.draws[gl_DrawID].texture;
}
