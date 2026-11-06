// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A world surface's pixel, for world.frag and, with ALPHA_TEST, world_alpha.frag.

#include "scene.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Materials scene_materials;
    LayerMaterials scene_layers;
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

// The normal n turned by a tangent-space normal, in the frame of the vertex's tangent.
vec3 TurnedNormal(vec3 n, vec3 tangent_normal) {
    if (tangent.xyz == vec3(0.0)) {
        return n;
    }
    const vec3 t = normalize(tangent.xyz);
    const vec3 b = cross(n, t) * sign(tangent.w);
    const vec3 surface_normal = normalize(tangent_normal);
    return normalize(t * surface_normal.x + b * surface_normal.y + n * surface_normal.z);
}

#ifdef ALPHA_TEST
// A screen door fades a surface in. The game's depth runs the other way from this one.
bool ScreenDoorShut(float threshold) {
    const float seed = 256.0 * (0.0771356 * gl_FragCoord.x + 0.00888381 * gl_FragCoord.y) *
                           (1.0 - gl_FragCoord.z) +
                       1e-7;
    return threshold < fract(fract(123456792.0 / seed) * seed);
}
#endif

// A layer's scaled normal in tangent space. A layer without a normal map is flat.
vec3 LayerNormal(LayerMaterials table, uint index, int layer, vec2 uv) {
    const uint map = table.layers[index].normal_map[layer];
    const vec3 direction =
        map != 0u
            ? 2.0 * Sample(map, Transformed(table.layers[index].normal_transform[layer], uv)).rgb -
                  1.0
            : vec3(0.0, 0.0, 1.0);
    return table.layers[index].normal_scale[layer] * direction;
}

vec4 LayerSurface(LayerMaterials table, uint index, int layer) {
    return vec4(table.layers[index].roughness[layer], table.layers[index].metallic[layer],
                table.layers[index].translucency[layer], table.layers[index].emissive[layer]);
}

// The game's G2_BG_Layer pixel program without its velocity: four layers mixed from the last to the
// first, each by a channel of the vertex colour and its colour's alpha. A layer with its blend flag
// multiplies the layers below by twice its colour and adds its normal to theirs.
void WriteLayers(uint index, uint hatching, vec3 n) {
    LayerMaterials table = scene_layers;
#ifdef ALPHA_TEST
    if (ScreenDoorShut(table.layers[index].punch_through)) {
        discard;
    }
#endif
    vec2 uv = texcoord.xy;
    const float parallax =
        mix(mix(mix(table.layers[index].parallax[3], table.layers[index].parallax[2],
                    vertex_color.z),
                table.layers[index].parallax[1], vertex_color.y),
            table.layers[index].parallax[0], vertex_color.x);
    const uint height_map = table.layers[index].normal_map[0];
    if (height_map != 0u && tangent.xyz != vec3(0.0) && parallax != 0.0) {
        const vec4 transform = table.layers[index].normal_transform[0];
        uv = (Parallax(height_map, Transformed(transform, uv), texcoord.zw * parallax, n.z) -
              transform.xy) /
             transform.zw;
    }
    vec3 albedo =
        Sample(table.layers[index].color_map[3],
               Transformed(table.layers[index].color_transform[3], uv))
            .rgb *
        table.layers[index].color_scale[3].rgb;
    vec3 tangent_normal = LayerNormal(table, index, 3, uv);
    vec4 surface = LayerSurface(table, index, 3);
    for (int layer = 2; layer >= 0; layer--) {
        const vec4 color = Sample(table.layers[index].color_map[layer],
                                  Transformed(table.layers[index].color_transform[layer], uv));
        const vec3 scaled = color.rgb * table.layers[index].color_scale[layer].rgb;
        const vec3 layer_normal = LayerNormal(table, index, layer, uv);
        const bool blends = table.layers[index].blend[layer] != 0u;
        const float weight = vertex_color[layer] * color.a;
        albedo = mix(albedo, blends ? 2.0 * albedo * scaled : scaled, weight);
        tangent_normal =
            mix(tangent_normal, blends ? tangent_normal + layer_normal : layer_normal, weight);
        surface = mix(surface, LayerSurface(table, index, layer), weight);
    }
    const vec2 repeat =
        vec2(table.layers[index].hatching_repeat_u, table.layers[index].hatching_repeat_v);
    g_albedo = vec4(Gradate(albedo), Sample(hatching, uv * repeat).r);
    g_normal = PackNormal(TurnedNormal(n, tangent_normal));
    g_surface = surface;
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
    if ((table.materials[index].flags & MaterialLayers) != 0u) {
        WriteLayers(table.materials[index].layer_material, table.materials[index].hatching, n);
        return;
    }
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
    if (color.a < color_scale.w || ScreenDoorShut(table.materials[index].punch_through)) {
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
    const float repeat = table.materials[index].hatching_repeat;
    g_albedo = vec4(albedo, Sample(table.materials[index].hatching, uv * repeat).r);
    g_normal = PackNormal(TurnedNormal(n, tangent_normal));
    g_surface = surface;
}
