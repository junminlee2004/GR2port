// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform textureCube cubes[];
layout(set = 0, binding = 2) uniform sampler clamp_sampler;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
    Decals scene_decals;
    Light light;
    vec2 target_inverse;
};

// character.vert's outputs. This draw's world_view is its world matrix.
layout(location = 0) in vec3 world_position;
layout(location = 1) in vec4 world_normal;
layout(location = 7) flat in uint draw_index;

layout(location = 0) out vec4 color;

vec4 CubeMap(uint handle, vec3 direction) {
    if (handle == 0u) {
        return vec4(0.0);
    }
    return texture(samplerCube(cubes[nonuniformEXT(handle)], clamp_sampler), direction);
}

// The share of a cube map's colour scale at a texel's alpha.
float Band(float alpha, float center) {
    return 1.0 - min(6.0 * abs(alpha - center), 1.0);
}

// The game's G2_CH_Interior pixel program: two cube maps seen from inside spheres around a point,
// the first turning with time, under a Fresnel-weighted reflection of the ambient radiance.
void main() {
    Characters table = scene_characters;
    const uint index = scene_draws.draws[draw_index].material - 1u;
    InteriorConstants m =
        InteriorConstants(uint64_t(scene_characters) + uint64_t(index) * CharacterBytes);

    const vec3 ray = normalize(world_position - m.view.xyz);
    const vec3 center = m.center.xyz;
    const vec3 closest = world_position + ray * dot(center - world_position, ray);
    const float off_axis = dot(center - closest, center - closest);
    const vec3 inner0 = normalize(closest + ray * sqrt(m.size_squared[0] + off_axis) - center);
    const vec3 inner1 = normalize(closest + ray * sqrt(m.size_squared[1] + off_axis) - center);
    const vec4 a = CubeMap(table.characters[index].normal_map, mat3(m.rotation) * inner0);
    const vec4 b = CubeMap(table.characters[index].reflection, inner1);
    const vec3 inner = a.rgb * (1.0 + m.band[0].rgb * Band(a.a, m.band[0].w)) +
                       b.rgb * (1.0 + m.band[1].rgb * Band(b.a, m.band[1].w));

    const vec3 n = normalize(world_normal.xyz);
    const float n_dot_v = min(max(-dot(ray, n), 0.0), 1.0);
    const float rough = m.roughness;
    const float gloss = 1.0 - rough;
    const float k =
        1.04 * (min(exp2(-9.28 * n_dot_v), gloss * gloss) * gloss - 0.0275 * rough) + 0.0442;
    const vec3 env = m.reflectance.rgb * (1.04 - 0.572 * rough - k) +
                     (0.022 * rough - 0.04 + k) * clamp(50.0 * m.reflectance.g, 0.0, 1.0);
    const float lod = clamp(6.0 + 1.15 * log2(max(rough, 1e-6)), 0.0, 6.5);
    vec3 radiance = vec3(0.0);
    if (light.radiance[0] != 0u) {
        radiance =
            textureLod(samplerCube(cubes[light.radiance[0]], clamp_sampler), reflect(ray, n), lod)
                .rgb;
    }
    const vec3 fresnel =
        m.fresnel_floor + (1.0 - m.fresnel_floor) * pow(max(1.0 - n_dot_v, 0.0), 5.0);
    const vec3 share = m.fresnel_scale * max(fresnel - m.fresnel_bias, vec3(0.0));
    color = vec4(inner + (radiance * env - inner) * share, 0.0);
}
