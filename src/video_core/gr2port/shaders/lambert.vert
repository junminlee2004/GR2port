// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
};

// The cascades' coordinates before the divide, the model's normal and the texture coordinates.
layout(location = 0) out vec4 shadow_clip[3];
layout(location = 3) out vec3 model_normal;
layout(location = 4) out vec2 texcoord;
layout(location = 5) flat out uint draw_index;

// The game's opaque/Lambert vertex programs: WorldViewProj, the three ShadowProj and the normal,
// all from the model's space.
void main() {
    Draws draws = scene_draws;
    draw_index = gl_BaseInstance;
    const uint64_t vertices = draws.draws[gl_BaseInstance].vertices;
    const uint stride = draws.draws[gl_BaseInstance].stride;
    const uint bones = draws.draws[gl_BaseInstance].bones;
    const uint64_t skin = draws.draws[gl_BaseInstance].skin;
    const uint64_t palette = draws.draws[gl_BaseInstance].palette;
    const vec3 unskinned = LoadPosition(vertices, stride, draws.draws[gl_BaseInstance].position);
    const vec4 model =
        vec4(bones != 0u ? SkinPosition(unskinned, skin, palette, bones) : unskinned, 1.0);
    vec4 clip = draws.draws[gl_BaseInstance].world_view_projection * model;
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;

    const uint index = draws.draws[gl_BaseInstance].material - 1u;
    LambertConstants m =
        LambertConstants(uint64_t(scene_characters) + uint64_t(index) * CharacterBytes);
    for (uint cascade = 0u; cascade < 3u; cascade++) {
        shadow_clip[cascade] = m.shadow_projection[cascade] * model;
    }
    const uint normal_element = draws.draws[gl_BaseInstance].normal;
    const vec3 normal = (normal_element >> 8) != 0u ? LoadNormal(vertices, stride, normal_element)
                                                    : vec3(0.0, 0.0, 1.0);
    model_normal = bones != 0u ? SkinDirection(normal, skin, palette, bones) : normal;
    texcoord = LoadTexcoord(vertices, stride, draws.draws[gl_BaseInstance].texcoord);
}
