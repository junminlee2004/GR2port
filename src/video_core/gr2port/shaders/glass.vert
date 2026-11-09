// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
};

layout(location = 0) out vec3 normal;
// The tangent, and in w the sign of the bitangent.
layout(location = 1) out vec4 tangent;
layout(location = 2) out vec3 view_position;
layout(location = 3) out vec2 texcoord;
layout(location = 4) flat out uint draw_index;

// The game's G2_BG_Glass vertex programs (0x1b14820, skinned 0x1b14ebc): the view-space frame and
// position. The pixel shader finds the shadow coordinates from the position.
void main() {
    Draws draws = scene_draws;
    draw_index = gl_InstanceIndex;
    const uint64_t vertices = draws.draws[gl_InstanceIndex].vertices;
    const uint stride = draws.draws[gl_InstanceIndex].stride;
    const uint bones = draws.draws[gl_InstanceIndex].bones;
    const uint64_t skin = draws.draws[gl_InstanceIndex].skin;
    const uint64_t palette = draws.draws[gl_InstanceIndex].palette;
    const vec3 unskinned = LoadPosition(vertices, stride, draws.draws[gl_InstanceIndex].position);
    const vec3 model = bones != 0u ? SkinPosition(unskinned, skin, palette, bones) : unskinned;
    const mat4 world_view = draws.draws[gl_InstanceIndex].world_view;
    view_position = (world_view * vec4(model, 1.0)).xyz;
    vec4 clip = draws.draws[gl_InstanceIndex].world_view_projection * vec4(model, 1.0);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
    texcoord = LoadTexcoord(vertices, stride, draws.draws[gl_InstanceIndex].texcoord);
    const vec3 n = LoadNormal(vertices, stride, draws.draws[gl_InstanceIndex].normal);
    const vec3 turned_normal = bones != 0u ? SkinDirection(n, skin, palette, bones) : n;
    normal = normalize(mat3(world_view) * turned_normal);
    const vec4 t = LoadTangent(vertices, stride, draws.draws[gl_InstanceIndex].tangent);
    const vec3 turned_tangent = bones != 0u ? SkinDirection(t.xyz, skin, palette, bones) : t.xyz;
    tangent = vec4(normalize(mat3(world_view) * turned_tangent), t.w);
}
