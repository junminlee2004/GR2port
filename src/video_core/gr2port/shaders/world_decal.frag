// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "scene.glsl"
#include "surface.glsl"

layout(set = 1, binding = 0) uniform sampler2D scene_depth;

layout(push_constant) uniform Constants {
    WorldDecals scene_decals;
    Light light;
};

layout(location = 0) flat in uint decal;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec4 g_normal;
layout(location = 2) out vec4 g_surface;

// The game's decal pixel program: the surface under the pixel, if the decal's box holds it, takes
// the decal's colour, normal and material, by the colour's alpha.
void main() {
    const float depth = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
    // Reverse depth to the game's OpenGL depth.
    const float distance_to_eye =
        1.0 / (light.inverse_projection.z * (1.0 - 2.0 * depth) + light.inverse_projection.w);
    const vec2 uv_on_screen = gl_FragCoord.xy / vec2(textureSize(scene_depth, 0));
    const vec2 ndc = vec2(2.0 * uv_on_screen.x - 1.0, 1.0 - 2.0 * uv_on_screen.y);
    const vec3 position =
        vec3(light.inverse_projection.xy * distance_to_eye * ndc, -distance_to_eye);

    WorldDecals decals = scene_decals;
    const mat4 view_to_decal = decals.decals[decal].view_to_decal;
    const vec3 place = (view_to_decal * vec4(position, 1.0)).xyz;
    const vec4 normal_transform = decals.decals[decal].normal_transform;
    const uint normal_map = decals.decals[decal].normal_map;
    vec2 normal_uv = Transformed(normal_transform, place.xy);
    const vec4 parallax = decals.decals[decal].parallax;
    if (parallax.w > 0.0 && normal_map != 0u) {
        const vec3 view = normalize(position);
        const vec3 along = mat3(view_to_decal) * view;
        normal_uv = Parallax(normal_map, normal_uv, parallax.w * along.xy / -along.z,
                             dot(parallax.xyz, view));
    }
    const vec2 uv = (normal_uv - normal_transform.xy) / normal_transform.zw;
    const vec4 color = Sample(decals.decals[decal].color_map,
                              Transformed(decals.decals[decal].color_transform, uv)) *
                       decals.decals[decal].color_scale;
    const vec3 tangent_normal =
        normal_map != 0u ? 2.0 * Sample(normal_map, normal_uv).rgb - 1.0 : vec3(0.0, 0.0, 1.0);
    // After the samples, whose derivatives need every pixel of their quad. The box holds x from 0
    // to 1, y from -1 to 0 and z from 0 to 1.
    if (depth <= 0.0 || any(greaterThan(abs(place - vec3(0.5, -0.5, 0.5)), vec3(0.5)))) {
        discard;
    }
    const vec3 n = normalize(-(mat3(decals.decals[decal].world_view) *
                               (decals.decals[decal].normal_scale.xyz * tangent_normal)));
    g_albedo = color;
    g_normal = vec4(PackNormal(n), 0.0, color.a);
    g_surface = vec4(decals.decals[decal].roughness, decals.decals[decal].metallic,
                     decals.decals[decal].translucency, color.a);
}
