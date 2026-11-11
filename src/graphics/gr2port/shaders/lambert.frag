// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map;
layout(set = 1, binding = 2) uniform sampler2DShadow last_shadow_map;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
};

// lambert.vert's outputs.
layout(location = 0) in vec4 shadow_clip[3];
layout(location = 3) in vec3 model_normal;
layout(location = 4) in vec2 texcoord;
layout(location = 5) flat in uint draw_index;

layout(location = 0) out vec4 color;

// How much of the key light reaches the point: one comparison in the first of the two nearer
// cascades whose map holds it, else in the last cascade, as the game's program takes it.
float KeyShadow() {
    for (uint cascade = 0u; cascade < 2u; cascade++) {
        const vec3 coord = shadow_clip[cascade].xyz / shadow_clip[cascade].w;
        if (min(coord.x, coord.y) >= 0.0 && max(coord.x, coord.y) <= 1.0) {
            return texture(shadow_map, vec4(coord.xy, float(cascade), coord.z));
        }
    }
    return texture(last_shadow_map, shadow_clip[2].xyz / shadow_clip[2].w);
}

// The game's opaque/Lambert pixel program: the colour map times the colour scale, lit by a
// half-Lambert term of the key light in the model's space, its shadow, and a floor of 0.2.
void main() {
    const uint index = scene_draws.draws[draw_index].material - 1u;
    LambertConstants m =
        LambertConstants(uint64_t(scene_characters) + uint64_t(index) * CharacterBytes);
    const uint handle = scene_draws.draws[draw_index].texture;
    const vec3 albedo =
        texture(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), texcoord).rgb;
    const float half_lambert = 0.5 - 0.5 * dot(normalize(model_normal), m.light_direction.xyz);
    color = vec4(albedo * m.color_scale.rgb * (0.7 * half_lambert * KeyShadow() + 0.2), 1.0);
}
