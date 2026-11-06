// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A world surface's pixel, for world.frag and, with ALPHA_TEST, world_alpha.frag.

#include "scene.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Materials scene_materials;
};

layout(location = 0) in vec3 normal;
layout(location = 1) in vec4 tangent;
layout(location = 2) in vec4 texcoord;
layout(location = 3) in vec4 vertex_color;
layout(location = 4) in vec3 gradation;
layout(location = 5) flat in uint draw_index;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec2 g_normal;
layout(location = 2) out vec4 g_surface;

vec4 Sample(uint handle, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), uv);
}

vec2 Transformed(vec4 transform, vec2 uv) {
    return transform.xy + transform.zw * uv;
}

// The game's packed normal: the direction of xy, with the length sqrt(0.5 - 0.5 z).
vec2 PackNormal(vec3 n) {
    return n.xy * inversesqrt(max(dot(n.xy, n.xy), 1e-6)) * sqrt(abs(0.5 - 0.5 * n.z));
}

// Parallax occlusion mapping through the normal map's alpha, in the normal map's coordinates: 8 to
// 14 steps along the direction by how far the surface turns from the eye, then the intersection of
// the last two steps' lines.
vec2 Parallax(uint handle, vec2 start, vec2 direction, float facing) {
    const int steps = int(14.0 - 6.0 * facing);
    const float step_size = 1.0 / float(steps);
    const vec2 dx = dFdx(start);
    const vec2 dy = dFdy(start);
    vec2 uv = start;
    float level = 1.0;
    float height = 0.0;
    float last_height = 1.0;
    for (int i = 0; i < steps; i++) {
        uv -= direction * step_size;
        height =
            textureGrad(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), uv, dx, dy).a;
        level -= step_size;
        if (height > level) {
            break;
        }
        last_height = height;
    }
    const float last_level = level + step_size;
    const float last_gap = last_level - last_height;
    const float gap = level - height;
    const float hit =
        last_gap != gap ? (level * last_gap - last_level * gap) / (last_gap - gap) : 0.0;
    return start - direction * (1.0 - hit);
}

// Half a hard light of the gradation over the colour.
vec3 Gradate(vec3 color) {
    const vec3 lighter = 1.0 - 2.0 * (1.0 - gradation) * (1.0 - color);
    const vec3 darker = 2.0 * gradation * color;
    return mix(color, mix(darker, lighter, step(vec3(0.5), color)), 0.5);
}

// The game's G2_BG_Main pixel programs without their velocity, one program for all their variants.
void main() {
    const float facing_sign = gl_FrontFacing ? 1.0 : -1.0;
    const uint material_index = scene_draws.draws[draw_index].material;
    const uint color_handle = scene_draws.draws[draw_index].texture;
    const vec4 color_scale = scene_draws.draws[draw_index].color_scale;
    const float length_squared = dot(normal, normal);
    // A mesh without normals faces the eye.
    vec3 n = length_squared > 0.0 ? normal * inversesqrt(length_squared) * facing_sign
                                  : vec3(0.0, 0.0, 1.0);
    if (material_index == 0u) {
#ifdef ALPHA_TEST
        discard;
#endif
        g_albedo = vec4(color_handle != 0u ? Sample(color_handle, texcoord.xy).rgb * color_scale.rgb
                                           : vec3(0.0),
                        1.0);
        g_normal = PackNormal(n);
        g_surface = vec4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    const uint index = material_index - 1u;
    Materials table = scene_materials;
    const bool layered = (table.materials[index].flags & MaterialLayered) != 0u;
    const float weight = layered ? vertex_color.a : 1.0;
    const uint normal_map = table.materials[index].normal_map;
    const vec4 normal_transform = table.materials[index].normal_transform;
    const vec2 start = Transformed(normal_transform, texcoord.xy);

    vec2 uv = texcoord.xy;
    const float parallax = mix(table.materials[index].layered_parallax,
                               table.materials[index].parallax, weight);
    if (normal_map != 0u && tangent.xyz != vec3(0.0) && parallax != 0.0) {
        uv = (Parallax(normal_map, start, texcoord.zw * parallax, n.z) - normal_transform.xy) /
             normal_transform.zw;
    }

    const vec4 color =
        Sample(color_handle, Transformed(table.materials[index].color_transform, uv));
#ifdef ALPHA_TEST
    if (color.a < color_scale.w) {
        discard;
    }
    // A screen door fades the surface in. The game's depth runs the other way from this one.
    const float seed = 256.0 * (0.0771356 * gl_FragCoord.x + 0.00888381 * gl_FragCoord.y) *
                           (1.0 - gl_FragCoord.z) +
                       1e-7;
    if (table.materials[index].punch_through < fract(fract(123456792.0 / seed) * seed)) {
        discard;
    }
#endif

    const vec4 mask =
        Sample(table.materials[index].mask, Transformed(table.materials[index].mask_transform, uv));
    vec3 albedo = color.rgb * color_scale.rgb;
    vec4 surface =
        mask * vec4(table.materials[index].roughness, table.materials[index].metallic,
                    table.materials[index].translucency, table.materials[index].emissive);
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (normal_map != 0u) {
        tangent_normal = 2.0 * Sample(normal_map, Transformed(normal_transform, uv)).rgb - 1.0;
    }
    if (layered) {
        const vec3 layer =
            Sample(table.materials[index].layered_color,
                   Transformed(table.materials[index].layered_color_transform, uv))
                .rgb *
            table.materials[index].layered_color_scale.rgb;
        albedo = mix(layer, albedo, weight);
        surface = mix(vec4(table.materials[index].layered_roughness,
                           table.materials[index].layered_metallic,
                           table.materials[index].layered_translucency,
                           table.materials[index].layered_emissive),
                      surface, weight);
        if (table.materials[index].layered_normal != 0u) {
            const vec3 layer_normal =
                2.0 *
                    Sample(table.materials[index].layered_normal,
                           Transformed(table.materials[index].layered_normal_transform, uv))
                        .rgb -
                1.0;
            tangent_normal = mix(layer_normal, tangent_normal, weight);
        }
        albedo *= vertex_color.rgb;
    }
    if ((table.materials[index].flags & MaterialGradation) != 0u) {
        albedo = Gradate(albedo);
    }
    if (tangent.xyz != vec3(0.0)) {
        const vec3 t = normalize(tangent.xyz);
        const vec3 b = cross(n, t) * sign(tangent.w);
        const vec3 surface_normal = normalize(tangent_normal);
        n = normalize(t * surface_normal.x + b * surface_normal.y + n * surface_normal.z);
    }
    const float repeat = table.materials[index].hatching_repeat;
    g_albedo = vec4(albedo, Sample(table.materials[index].hatching, uv * repeat).r);
    g_normal = PackNormal(n);
    g_surface = surface;
}
