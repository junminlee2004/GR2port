// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// A world surface's pixel, for world.frag and, with ALPHA_TEST, world_alpha.frag.

#include "reconvergence.glsl"
#include "scene.glsl"
#include "surface.glsl"

#include "texture_slots.glsl"
layout(set = 0, binding = 0) uniform textureCube cubes[];
layout(set = 0, binding = 2) uniform sampler clamp_sampler;

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

// The game's G2_BG_Gem pixel program without its velocity: the colour map shows what the view ray
// meets after it refracts through the surface, over the gem's own colour by the map's alpha.
void WriteGem(uint index, vec3 n) {
    GemMaterials table = GemMaterials(scene_layers);
#ifdef ALPHA_TEST
    if (ScreenDoorShut(table.gems[index].punch_through)) {
        discard;
    }
#endif
    const uint normal_map = table.gems[index].normal_map;
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (normal_map != 0u) {
        const vec3 map = Sample(normal_map, texcoord.xy).rgb;
        tangent_normal = vec3(table.gems[index].normal_scale * (2.0 * map.xy - 1.0), map.z);
    }
    const vec3 turned = TurnedNormal(n, tangent_normal);
    const vec3 ray = refract(normalize(gradation), turned, table.gems[index].refraction);
    const vec2 place = table.gems[index].projection * ray.xy / -ray.z + table.gems[index].offset;
    const vec2 lookup = vec2(0.5 + 0.5 * table.gems[index].scale.x * place.x,
                             0.5 - 0.5 * table.gems[index].scale.y * place.y);
    const vec4 color = Sample(table.gems[index].color_map, lookup);
    g_albedo = vec4(mix(table.gems[index].color.rgb, table.gems[index].color_scale.rgb * color.rgb,
                        color.a),
                    0.0);
    g_normal = PackNormal(turned);
    g_surface = vec4(table.gems[index].roughness, table.gems[index].metallic,
                     table.gems[index].translucency, table.gems[index].emissive);
}

// HLSL's fmod, as the game's program takes it: the remainder of x over y, with the sign of x.
vec3 Remainder(vec3 x, float y) {
    const vec3 r = abs(y) * fract(abs(x * (1.0 / y)));
    return mix(r, -r, lessThan(x, vec3(0.0)));
}

// The game's G2_BG_Interior pixel program without its velocity: where the colour map is clear, a
// building's window shows a room. The model's space is a lattice of rooms; the view ray from the
// pixel's place in its room meets the walls in the room cube map, unless a plane stands before them
// at its depth behind the window. The room cube map's alpha lights the room.
void WriteRooms(uint index, uint color_handle, vec3 color_scale, vec3 n) {
    RoomMaterials table = RoomMaterials(scene_layers);
#ifdef ALPHA_TEST
    if (ScreenDoorShut(table.rooms[index].punch_through)) {
        discard;
    }
#endif
    const vec2 uv = texcoord.xy;
    // world.vert's position and normal in the model's space.
    const vec3 position = gradation;
    const vec3 model_normal = normalize(vertex_color.xyz);
    const vec3 ray = position - table.rooms[index].eye.xyz;
    const uint normal_map = table.rooms[index].normal_map;
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    if (normal_map != 0u) {
        tangent_normal = normalize(2.0 * Sample(normal_map, uv).rgb - 1.0);
    }
    const float distortion = table.rooms[index].distortion;
    const vec4 lattice = table.rooms[index].lattice;
    // The pixel's place in its room, from -1 to 1 on each axis, moved across by the normal map.
    const vec3 place =
        2.0 * Remainder(position + lattice.xyz + lattice.w, lattice.w) * (1.0 / lattice.w) - 1.0 +
        vec3(distortion * tangent_normal.xy, 0.0);
    // How far along the ray the room's walls lie.
    const vec3 inverse = 1.0 / ray;
    const vec3 walls = abs(inverse) - place * inverse;
    const float leave = min(min(walls.x, walls.y), walls.z);
    vec4 room = vec4(0.0);
    const uint room_map = table.rooms[index].room_map;
    if (room_map != 0u) {
        room = texture(samplerCube(cubes[nonuniformEXT(room_map & TextureSlotMask)], clamp_sampler),
                       place + leave * ray, table.rooms[index].smokiness);
    }
    const vec4 plane = table.rooms[index].plane;
    const uint plane_map = table.rooms[index].plane_map;
    const float to_plane = -plane.z / dot(ray, model_normal);
    if (plane_map != 0u && lattice.w * leave > to_plane) {
        const vec3 hit = position + to_plane * ray;
        const float across = abs(model_normal.x) < abs(model_normal.y) ? hit.x : hit.z;
        const vec2 plane_uv = vec2(plane.x + plane.w * across, plane.y - plane.w * hit.y) +
                              0.1 * distortion * tangent_normal.xy;
        const vec4 sheet = Sample(plane_map, plane_uv);
        room = vec4(mix(room.rgb, sheet.rgb, sheet.a), room.a * (1.0 - sheet.a));
    }
    const vec4 color = Sample(color_handle, uv);
    g_albedo = vec4(mix(room.rgb, color_scale * color.rgb, color.a), 1.0);
    g_normal = PackNormal(TurnedNormal(n, tangent_normal));
    g_surface = vec4(mix(table.rooms[index].roughness[1], table.rooms[index].roughness[0], color.a),
                     mix(table.rooms[index].metallic[1], table.rooms[index].metallic[0], color.a),
                     0.0, table.rooms[index].emissive * room.a * (1.0 - color.a));
}

// The game's G2_BG_Main pixel programs without their velocity, one program for all their variants.
void Shade(uint draw) {
    const float facing_sign = gl_FrontFacing ? 1.0 : -1.0;
    const uint material_index = scene_draws.draws[draw].material;
    const uint color_handle = scene_draws.draws[draw].texture;
    const vec4 color_scale = scene_draws.draws[draw].color_scale;
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
    if ((table.materials[index].flags & MaterialGem) != 0u) {
        WriteGem(table.materials[index].layer_material, n);
        return;
    }
    if ((table.materials[index].flags & MaterialRooms) != 0u) {
        WriteRooms(table.materials[index].layer_material, color_handle, color_scale.rgb, n);
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

// A wave can hold the pixels of several draws. It shades one draw's pixels at a time, so the draw's
// constants and textures load once for the wave, into scalar registers. It takes at most one turn a
// lane, whatever the driver makes of the turns: a lane no turn reached shades its own draw.
void main() MAXIMALLY_RECONVERGES {
    for (uint turn = 0u; turn < gl_SubgroupSize; turn++) {
        const uint draw = subgroupBroadcastFirst(draw_index);
        if (draw == draw_index) {
            Shade(draw);
            return;
        }
    }
    Shade(draw_index);
}
