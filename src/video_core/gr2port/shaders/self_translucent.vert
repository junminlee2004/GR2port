// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
};

layout(location = 0) flat out uint draw_index;

// The game's SelfTranslucent vertex programs: the skin moved along its normal. A draw's world_view
// holds the game's WorldViewProj, and the first word of its world_view_projection the distance.
void main() {
    Draws draws = scene_draws;
    draw_index = gl_InstanceIndex;
    const uint64_t vertices = draws.draws[gl_InstanceIndex].vertices;
    const uint stride = draws.draws[gl_InstanceIndex].stride;
    const uint bones = draws.draws[gl_InstanceIndex].bones;
    const vec3 normal = LoadNormal(vertices, stride, draws.draws[gl_InstanceIndex].normal);
    vec3 model = LoadPosition(vertices, stride, draws.draws[gl_InstanceIndex].position) +
                 draws.draws[gl_InstanceIndex].world_view_projection[0][0] * normal;
    if (bones != 0u) {
        model = SkinPosition(model, draws.draws[gl_InstanceIndex].skin,
                             draws.draws[gl_InstanceIndex].palette, bones);
    }
    vec4 clip = draws.draws[gl_InstanceIndex].world_view * vec4(model, 1.0);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
}
