// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "surface.glsl"

layout(set = 0, binding = 0) uniform textureCube cubes[];
layout(set = 0, binding = 2) uniform sampler clamp_sampler;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map;
layout(set = 1, binding = 2) uniform sampler2DShadow last_shadow_map;
layout(set = 2, binding = 0) uniform sampler2D behind_glass;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    GlassMaterials scene_materials;
    Light light;
};

layout(location = 0) in vec3 normal;
layout(location = 1) in vec4 tangent;
layout(location = 2) in vec3 view_position;
layout(location = 3) in vec2 texcoord;
layout(location = 4) flat in uint draw_index;

layout(location = 0) out vec4 color;

// What the program writes: the pane over the refracted copy of the scene, as the game draws it;
// or, for the two blends that need no copy, the share of the scene that shows through the pane,
// then the pane's own light.
layout(constant_id = 0) const uint GlassOutput = 0u;
const uint RefractedOutput = 0u;
const uint ThroughOutput = 1u;

const float Pi = 3.14159265;

// How much of the key light reaches the point: one tap of its cascade.
float KeyShadow() {
    const float depth = -view_position.z;
    const uint cascade = depth < light.split_depths.y ? 0u : depth < light.split_depths.z ? 1u : 2u;
    const vec4 clip = light.shadow_projection[cascade] * vec4(view_position, 1.0);
    const vec3 coord = clip.xyz / clip.w;
    if (cascade == 2u) {
        return texture(last_shadow_map, coord);
    }
    return texture(shadow_map, vec4(coord.xy, float(cascade), coord.z));
}

// Where a ray that refracts at the surface lands on the screen.
vec2 Refracted(vec3 ray, vec3 n, float ratio, vec2 projection) {
    const vec3 bent = refract(ray, n, ratio);
    return vec2(0.5) + vec2(0.5, -0.5) * projection * bent.xy / -bent.z;
}

// The game's G2_BG_Glass pixel program (0x1b8712c): the scene behind the pane, refracted through
// its normal map and tinted, under a Fresnel-weighted reflection of the ambient radiance and the
// key light's highlight.
void main() {
    GlassMaterials materials = scene_materials;
    const uint index = scene_draws.draws[draw_index].material - 1u;
    const vec3 ray = normalize(view_position);
    const vec3 face_normal = normalize(normal);
    const vec3 face_tangent = normalize(tangent.xyz);
    const vec3 face_bitangent = sign(tangent.w) * cross(face_normal, face_tangent);
    const uint normal_map = scene_draws.draws[draw_index].texture;
    const vec2 normal_uv = Transformed(materials.materials[index].normal_transform, texcoord);
    const vec3 texel = normal_map != 0u ? Sample(normal_map, normal_uv).rgb : vec3(0.5, 0.5, 1.0);
    const float normal_scale = materials.materials[index].normal_scale;
    const vec3 surface_normal = normalize(vec3(normal_scale * (2.0 * texel.xy - 1.0), texel.z));
    const vec3 n = normalize(surface_normal.x * face_tangent + surface_normal.y * face_bitangent +
                             surface_normal.z * face_normal);
    const float n_dot_v = min(max(-dot(ray, n), 0.0), 1.0);
    const vec3 reflectance = materials.materials[index].reflectance.rgb;
    const vec3 share = reflectance + (1.0 - reflectance) * pow(max(1.0 - n_dot_v, 0.0), 5.0);
    if (GlassOutput == ThroughOutput) {
        // What the reflection leaves of the scene, tinted.
        color = vec4((1.0 - share) * materials.materials[index].translucency.rgb, 1.0);
        return;
    }

    // In the light pass the target already holds the scene through the pane, so nothing is read.
    vec3 behind = vec3(0.0);
    if (GlassOutput == RefractedOutput) {
        const vec4 refraction = materials.materials[index].refraction;
        const vec2 projection = materials.materials[index].projection;
        if (refraction.w > 0.0) {
            // Each colour bends by its own ratio.
            behind.r = textureLod(behind_glass, Refracted(ray, n, refraction.r, projection), 0.0).r;
            behind.g = textureLod(behind_glass, Refracted(ray, n, refraction.g, projection), 0.0).g;
            behind.b = textureLod(behind_glass, Refracted(ray, n, refraction.b, projection), 0.0).b;
        } else {
            behind = textureLod(behind_glass, Refracted(ray, n, refraction.r, projection), 0.0).rgb;
        }
    }

    // Key light: GGX and Schlick.
    const vec3 travel = materials.materials[index].light_direction.xyz;
    const vec3 halfway = normalize(-travel - ray);
    const float n_dot_l = min(max(-dot(travel, n), 0.0), 1.0);
    const float n_dot_h = clamp(dot(n, halfway), 0.0, 1.0);
    const float rough = max(materials.materials[index].roughness, 1e-6);
    const float a2 = rough * rough;
    const float d_term = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    const float ggx = a2 / (Pi * d_term * d_term);
    const float visibility =
        1.0 / ((n_dot_v + sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v)) *
               (n_dot_l + sqrt(a2 + (1.0 - a2) * n_dot_l * n_dot_l)));
    const float highlight_fresnel = pow(clamp(1.0 + dot(ray, halfway), 0.0, 1.0), 5.0);
    const vec3 highlight = (reflectance + (1.0 - reflectance) * highlight_fresnel) * ggx *
                           visibility * n_dot_l * KeyShadow() *
                           materials.materials[index].light_color.rgb;

    // The ambient radiance along the reflection, in world space.
    const float gloss = 1.0 - rough;
    const float k =
        1.04 * (min(exp2(-9.28 * n_dot_v), gloss * gloss) * gloss - 0.0275 * rough) + 0.0442;
    const vec3 env = reflectance * (1.04 - 0.572 * rough - k) +
                     (0.022 * rough - 0.04 + k) * clamp(50.0 * reflectance.g, 0.0, 1.0);
    vec3 radiance = vec3(0.0);
    if (light.radiance[0] != 0u) {
        const mat3 view_to_world = mat3(materials.materials[index].view_inverse);
        const vec3 reflected =
            reflect(normalize(view_to_world * ray), normalize(view_to_world * n));
        const float lod = clamp(6.0 + 1.15 * log2(rough), 0.0, 6.5);
        radiance =
            textureLod(samplerCube(cubes[light.radiance[0]], clamp_sampler), reflected, lod).rgb;
    }

    const vec3 seen = materials.materials[index].translucency.rgb * behind;
    color = vec4(seen + share * (radiance * env + highlight - seen), 1.0);
}
