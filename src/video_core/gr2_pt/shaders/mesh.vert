// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Shadows shadows;
};

layout(location = 0) out vec3 view_position;
layout(location = 1) out vec2 texcoord;
layout(location = 2) flat out vec3 color_scale;
layout(location = 3) flat out uint texture_index;

void main() {
    Draws draws = scene_draws;
    const uint64_t vertices = draws.draws[gl_DrawID].vertices;
    const uint stride = draws.draws[gl_DrawID].stride;
    vec3 model = LoadPosition(vertices, stride, draws.draws[gl_DrawID].position);
    const uint bones = draws.draws[gl_DrawID].bones;
    if (bones != 0u) {
        model = SkinPosition(model, draws.draws[gl_DrawID].skin, draws.draws[gl_DrawID].palette,
                             bones);
    }
    const vec4 position = vec4(model, 1.0);
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
