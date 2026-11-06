// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform textureCube cubes[];
layout(set = 0, binding = 2) uniform sampler clamp_sampler;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map;
layout(set = 1, binding = 1) uniform sampler2DArray shadow_depth;
layout(set = 2, binding = 0) uniform sampler2D scene_depth;
layout(set = 2, binding = 1) uniform sampler2D surface_albedo;
layout(set = 2, binding = 2) uniform sampler2D surface_normal;
layout(set = 2, binding = 3) uniform sampler2D surface_material;
layout(set = 2, binding = 4) uniform sampler2D world_lines;

layout(push_constant) uniform Constants {
    Light light;
};

layout(location = 0) out vec4 color;

const float Pi = 3.14159265;
const vec3 LumaWeights = vec3(0.299, 0.587, 0.114);
const float ShadowFadeRange = 0.1;
// The distance the depth under a surface is read behind it, in the first cascade and the others.
const float ThicknessOffsets[2] = float[2](0.012, 0.08);
const float TranslucencyRange = 1.2;
const float AmbientTranslucency = 2.0;

vec3 ShadowCoord(uint cascade, vec3 position) {
    const vec4 coord = light.shadow_projection[cascade] * vec4(position, 1.0);
    return coord.xyz / coord.w;
}

float ShadowTap(uint cascade, vec3 coord, vec2 offset) {
    return texture(shadow_map, vec4(coord.xy + offset, float(cascade), coord.z));
}

// How much of the key light reaches a point: four taps in the first two cascades, one in the last.
float KeyShadow(uint cascade, vec3 position) {
    const vec3 coord = ShadowCoord(cascade, position);
    if (cascade == 2u) {
        return ShadowTap(cascade, coord, vec2(0.0));
    }
    return 0.25 * (ShadowTap(cascade, coord, vec2(1.3e-4, 3.64e-4)) +
                   ShadowTap(cascade, coord, vec2(3.56e-4, 1.01e-4)) +
                   ShadowTap(cascade, coord, vec2(-2.24e-4, 7.96e-5)) +
                   ShadowTap(cascade, coord, vec2(2.02e-4, -4.23e-4)));
}

// The distance the key light travels through the surface to reach a point.
float Thickness(uint cascade, vec3 position, vec3 normal) {
    const float offset = ThicknessOffsets[min(cascade, 1u)];
    const vec3 coord = ShadowCoord(cascade, position - offset * normal);
    const float depth = textureLod(shadow_depth, vec3(coord.xy, float(cascade)), 0.0).r;
    const vec4 caster =
        light.inverse_shadow_projection[cascade] * vec4(coord.xy, depth, 1.0);
    return length(caster.xyz / caster.w - position);
}

// The share of light that passes through a thickness: a sum of six Gaussians.
float Transmittance(float s) {
    const float s2 = -s * s;
    return 0.233 * exp(s2 / 0.0064) + 0.1 * exp(s2 / 0.0484) + 0.118 * exp(s2 / 0.187) +
           0.113 * exp(s2 / 0.567) + 0.358 * exp(s2 / 1.99) + 0.078 * exp(s2 / 7.41);
}

vec3 Irradiance(vec3 direction) {
    if (light.irradiance[0] == 0u) {
        return vec3(0.0);
    }
    const vec3 first =
        textureLod(samplerCube(cubes[light.irradiance[0]], clamp_sampler), direction, 0.0).rgb;
    return light.irradiance[1] == 0u
               ? first
               : mix(first,
                     textureLod(samplerCube(cubes[light.irradiance[1]], clamp_sampler), direction,
                                0.0)
                         .rgb,
                     light.ibl_blend);
}

vec3 Radiance(vec3 direction, float lod) {
    if (light.radiance[0] == 0u) {
        return vec3(0.0);
    }
    const vec3 first =
        textureLod(samplerCube(cubes[light.radiance[0]], clamp_sampler), direction, lod).rgb;
    return light.radiance[1] == 0u
               ? first
               : mix(first,
                     textureLod(samplerCube(cubes[light.radiance[1]], clamp_sampler), direction,
                                lod)
                         .rgb,
                     light.ibl_blend);
}

// What the occluders leave of the ambient light at a position.
float OccluderShade(vec3 position, float strength) {
    float shade = 1.0;
    for (uint i = 0u; i < light.occluders; i++) {
        const vec4 sphere = light.occluder_spheres[i];
        const vec3 offset = position - sphere.xyz;
        const float along = dot(light.occluder_direction.xyz, offset);
        const float outside = dot(offset, offset) - sphere.w * sphere.w;
        const float clear = along > 0.0
                                ? (outside >= -0.0001 ? 1.0 : 0.0)
                                : clamp(light.occluder_penumbra / along * (along * along - outside),
                                        0.0, 1.0);
        const float blocked = max(0.8 - clear, 0.0) *
                              (1.0 - min(light.occluder_range * abs(along), 1.0));
        shade *= 1.0 - blocked * strength;
    }
    return shade;
}

// Bands above 1 / toning: each whole step keeps only the top of its range.
vec3 Tone(vec3 c) {
    const vec3 t = c * light.toning;
    const vec3 f = fract(t);
    const vec3 f2 = f * f;
    return mix(t, t - f + f2 * f2, step(vec3(1.0), t)) / light.toning;
}

// The game's LightPass (key light only), IBL and Resolve programs in one pass, without ambient
// occlusion and screen-space shadows.
void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const float depth = texelFetch(scene_depth, pixel, 0).r;
    if (depth <= 0.0) {
        discard;
    }
    const vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene_depth, 0));
    // Reverse depth to the game's OpenGL depth.
    const float distance_to_eye =
        1.0 / (light.inverse_projection.z * (1.0 - 2.0 * depth) + light.inverse_projection.w);
    const vec2 ndc = vec2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
    const vec3 position =
        vec3(light.inverse_projection.xy * distance_to_eye * ndc, -distance_to_eye);
    const vec4 albedo_hatching = texelFetch(surface_albedo, pixel, 0);
    const vec3 albedo = albedo_hatching.rgb;
    const vec2 packed_normal = texelFetch(surface_normal, pixel, 0).rg;
    const vec4 surface = texelFetch(surface_material, pixel, 0);
    const float translucency = surface.b;

    const float packed_squared = dot(packed_normal, packed_normal);
    const vec3 normal =
        normalize(vec3(2.0 * packed_normal * sqrt(max(1.0 - packed_squared, 0.0)),
                       1.0 - 2.0 * packed_squared));

    // Distant surfaces lose their highlights.
    const float highlight_fade =
        smoothstep(light.specular_fade.x, light.specular_fade.y, distance_to_eye);
    const float roughness = max(mix(surface.r, 0.7, highlight_fade), 1e-6);
    const float metallic = mix(surface.g, 0.3, highlight_fade);
    const vec3 reflectance = albedo * metallic;
    const vec3 diffuse = albedo - reflectance;

    const uint cascade = distance_to_eye < light.split_depths.y   ? 0u
                         : distance_to_eye < light.split_depths.z ? 1u
                                                                  : 2u;
    const float shadow_fade =
        1.0 - clamp((distance_to_eye - light.shadow_fade_start) * ShadowFadeRange, 0.0, 1.0);
    const float visible = 1.0 - shadow_fade * (1.0 - KeyShadow(cascade, position));

    // Key light: GGX and Schlick for the highlight, Lambert for the rest.
    const vec3 to_point = normalize(position);
    const vec3 travel = light.light_direction.xyz;
    const float light_on_normal = dot(travel, normal);
    const float n_dot_l = min(max(-light_on_normal, 0.0), 1.0);
    const vec3 halfway = normalize(-to_point - travel);
    const float n_dot_h = clamp(dot(normal, halfway), 0.0, 1.0);
    const float n_dot_v = min(max(-dot(to_point, normal), 0.0), 1.0);
    const float fresnel = pow(clamp(1.0 + dot(to_point, halfway), 0.0, 1.0), 5.0);
    const float a2 = roughness * roughness;
    const float d_term = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    const float ggx = a2 / (Pi * d_term * d_term);
    const float visibility =
        1.0 / ((n_dot_v + sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v)) *
               (n_dot_l + sqrt(a2 + (1.0 - a2) * n_dot_l * n_dot_l)));
    const vec3 highlight = (reflectance + (1.0 - reflectance) * fresnel) * ggx * visibility;
    vec3 lit = light.light_color.rgb * visible * n_dot_l * (highlight + diffuse / Pi);

    // Light through thin surfaces, from behind them.
    if (translucency > 0.0) {
        const float s = TranslucencyRange * Thickness(cascade, position, normal) *
                        (1.0 - translucency);
        lit += light.light_color.rgb * diffuse * translucency * Transmittance(s) *
               max(0.3 + light_on_normal, 0.0);
    }
    lit = max(lit, vec3(0.0)) + 6.0 * surface.a * albedo;

    // Ambient light from the sky's cube maps.
    const vec3 sky_normal = normalize(mat3(light.view_to_sky) * normal);
    vec3 ambient = diffuse * Irradiance(sky_normal);
    if (translucency > 0.0) {
        ambient += AmbientTranslucency * translucency * diffuse * Irradiance(-sky_normal);
    }
    const vec3 sky_ray = normalize(mat3(light.view_to_sky) * to_point);
    const float n_dot_v_sky = min(max(-dot(sky_ray, sky_normal), 0.0), 1.0);
    const float rough = max(roughness, 0.001);
    const float lod = clamp(6.0 + 1.15 * log2(rough), 0.0, 6.5);
    const float inverse_rough = 1.0 - rough;
    const float a004 =
        min(inverse_rough * inverse_rough, pow(2.0, -9.28 * n_dot_v_sky)) * inverse_rough -
        0.0275 * rough + 0.0425;
    const float scale = 1.04 - 0.572 * rough - 1.04 * a004;
    const float bias = (0.022 * rough - 0.04 + 1.04 * a004) *
                       clamp(50.0 * reflectance.g, 0.0, 1.0);
    ambient += (reflectance * scale + bias) * Radiance(reflect(sky_ray, sky_normal), lod);

    // Comic shading: the occluders' shade, bands, then the fades to the gradation and background.
    ambient *= light.ibl_intensity;
    const float strength = light.occluder_intensity * max(1.0 - dot(lit, LumaWeights), 0.0);
    ambient *= OccluderShade(position, strength);
    vec3 shaded = Tone(max(lit + ambient, vec3(0.0)));
    const float gradation_amount =
        clamp((distance_to_eye - light.gradation_start) * light.gradation_range, 0.0, 1.0);
    const vec3 gradation = mix(mix(light.gradation_middle.rgb, light.gradation_low.rgb,
                                   max(-sky_ray.y, 0.0)),
                               light.gradation_high.rgb, max(sky_ray.y, 0.0));
    shaded = mix(shaded, gradation, gradation_amount);
    const float background_amount =
        clamp((distance_to_eye - light.background_start) * light.background_range, 0.0, 1.0) *
        light.background_intensity;
    if (background_amount > 0.0 && light.radiance[0] != 0u) {
        const vec3 background = textureLod(samplerCube(cubes[light.radiance[0]], clamp_sampler),
                                           sky_ray, light.background_lod)
                                    .rgb;
        shaded = mix(shaded, background, background_amount);
    }
    // The lines darken or lighten by the contour colour, with distance by another share.
    const float line_share = mix(
        light.contour_blend, light.contour_distant_blend,
        clamp((distance_to_eye - light.contour_start) * light.contour_range, 0.0, 1.0));
    const float line = clamp(texelFetch(world_lines, pixel, 0).r, 0.0, 1.0) * line_share;
    color = vec4(shaded * (1.0 + (2.0 * light.contour_color.rgb - 1.0) * line), 1.0);
}
