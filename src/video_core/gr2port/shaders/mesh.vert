// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Shadows shadows;
    Decals scene_decals;
};

layout(location = 0) out vec3 view_position;
layout(location = 1) out vec2 texcoord;
layout(location = 2) flat out vec3 color_scale;
layout(location = 3) flat out uint texture_index;
layout(location = 4) out vec2 decal_texcoord;
layout(location = 5) flat out uint decal_index;

// Where a model position falls on a decal, and in z its squared distance from the decal's position.
// The decal faces its target direction with its right side horizontal.
vec3 OnDecal(vec3 model, vec3 position, vec3 target, vec2 scale) {
    const vec3 direction = normalize(target);
    const vec3 right = normalize(vec3(direction.z, 0.0, -direction.x));
    const vec3 up = normalize(cross(direction, right));
    const vec3 offset = model - position;
    return vec3(scale.x * dot(offset, right) + 0.5, 0.5 - scale.y * dot(offset, up),
                dot(offset, offset));
}

// The game's CH_Main vertex programs: the nearest decal's coordinates, from the unskinned position.
vec2 DecalTexcoord(vec3 model, uint index) {
    vec3 nearest = OnDecal(model, scene_decals.decals[index].positions[0].xyz,
                           scene_decals.decals[index].targets[0].xyz,
                           scene_decals.decals[index].scales[0]);
    for (uint i = 1u; i < 3u; i++) {
        const vec3 other = OnDecal(model, scene_decals.decals[index].positions[i].xyz,
                                   scene_decals.decals[index].targets[i].xyz,
                                   scene_decals.decals[index].scales[i]);
        if (nearest.z >= other.z) {
            nearest = other;
        }
    }
    return nearest.xy;
}

void main() {
    Draws draws = scene_draws;
    const uint64_t vertices = draws.draws[gl_DrawID].vertices;
    const uint stride = draws.draws[gl_DrawID].stride;
    const vec3 unskinned = LoadPosition(vertices, stride, draws.draws[gl_DrawID].position);
    vec3 model = unskinned;
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
    decal_index = draws.draws[gl_DrawID].decal;
    decal_texcoord = decal_index != 0u ? DecalTexcoord(unskinned, decal_index - 1u) : vec2(0.0);
}
