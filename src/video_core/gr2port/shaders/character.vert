// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
    Decals scene_decals;
    Light light;
    vec2 target_inverse;
};

layout(location = 0) out vec3 view_position;
// The normal, and in w the rim's share of the bend.
layout(location = 1) out vec4 normal;
// The tangent, and in w the sign of the bitangent.
layout(location = 2) out vec4 tangent;
layout(location = 3) out vec2 texcoord;
layout(location = 4) out vec2 decal_texcoord;
layout(location = 5) out vec2 model_xy;
layout(location = 6) out vec4 vertex_color;
layout(location = 7) flat out uint draw_index;

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

// The nearest decal's coordinates, from the unskinned position.
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

// The game's G2_CH_Main vertex programs: the view-space frame, with the normal bent toward a point
// past the material's distance from it, the decal's coordinates and the model's height for the
// reflection map.
void main() {
    Draws draws = scene_draws;
    draw_index = gl_DrawID;
    const uint64_t vertices = draws.draws[gl_DrawID].vertices;
    const uint stride = draws.draws[gl_DrawID].stride;
    const uint bones = draws.draws[gl_DrawID].bones;
    const uint64_t skin = draws.draws[gl_DrawID].skin;
    const uint64_t palette = draws.draws[gl_DrawID].palette;
    const vec3 unskinned = LoadPosition(vertices, stride, draws.draws[gl_DrawID].position);
    const vec3 model = bones != 0u ? SkinPosition(unskinned, skin, palette, bones) : unskinned;
    const mat4 world_view = draws.draws[gl_DrawID].world_view;
    view_position = (world_view * vec4(model, 1.0)).xyz;
    vec4 clip = draws.draws[gl_DrawID].world_view_projection * vec4(model, 1.0);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;

    const vec4 transform = draws.draws[gl_DrawID].texcoord_transform;
    texcoord = transform.xy + transform.zw * LoadTexcoord(vertices, stride,
                                                          draws.draws[gl_DrawID].texcoord);
    const uint decal_index = draws.draws[gl_DrawID].decal;
    decal_texcoord = decal_index != 0u ? DecalTexcoord(unskinned, decal_index - 1u) : vec2(0.0);
    model_xy = unskinned.xy;

    const uint normal_element = draws.draws[gl_DrawID].normal;
    vec3 n = (normal_element >> 8) != 0u ? LoadNormal(vertices, stride, normal_element)
                                         : vec3(0.0, 0.0, 1.0);
    n = bones != 0u ? SkinDirection(n, skin, palette, bones) : n;
    n = mat3(world_view) * n;
    float rim = 1.0;
    const uint index = draws.draws[gl_DrawID].material - 1u;
    const float bias = scene_characters.characters[index].bend_bias;
    if (bias > 0.0) {
        const vec3 offset = view_position - scene_characters.characters[index].bend_position.xyz;
        const float offset_length = length(offset);
        const float range = scene_characters.characters[index].bend_range;
        const float past = max(min(range * offset_length, 1.0) - bias, 0.0);
        const float share = past / (1.0 - bias);
        rim = min(share + 0.3, 1.0);
        n = mix(offset / offset_length, n, share);
    }
    normal = vec4(normalize(n), rim);

    const uint tangent_element = draws.draws[gl_DrawID].tangent;
    tangent = vec4(1.0, 0.0, 0.0, 1.0);
    if ((tangent_element >> 8) != 0u) {
        const vec4 t = LoadTangent(vertices, stride, tangent_element);
        const vec3 turned = bones != 0u ? SkinDirection(t.xyz, skin, palette, bones) : t.xyz;
        tangent = vec4(normalize(mat3(world_view) * turned), -t.w);
    }
    const uint color_element = draws.draws[gl_DrawID].vertex_color;
    vertex_color = (color_element >> 8) != 0u ? LoadVertexColor(vertices, stride, color_element)
                                              : vec4(1.0);
}
