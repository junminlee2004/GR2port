// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

#ifdef CHARACTER_MOTION
// The clip positions of the Character list's vertices: x, y and w of each.
layout(buffer_reference, std430, buffer_reference_align = 4) buffer ClipHistory {
    float values[];
};
#endif

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
    Decals scene_decals;
    Light light;
    vec2 target_inverse;
#ifdef CHARACTER_MOTION
    // The frame's clip history, and the last frame's, which holds that frame's positions when
    // last_clip_valid is not 0.
    layout(offset = 60) uint last_clip_valid;
    layout(offset = 64) ClipHistory clip_history;
    layout(offset = 72) ClipHistory last_clip_history;
#endif
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
#ifdef CHARACTER_MOTION
// The clip position's x, y and w, and the last frame's.
layout(location = 8) out vec3 current_clip;
layout(location = 9) out vec3 last_clip;
#endif

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

#ifdef CHARACTER_MOTION
// Keeps the vertex's clip position in the frame's clip history, and finds the last frame's: in that
// frame's history, where the last frame's matrix and palette of the shape the draw took over from
// put its unskinned position (ClipSwap), or else where the view position was under the last frame's
// camera.
void KeepClip(vec4 clip, vec3 view, uint character, vec3 unskinned, uint64_t skin,
              uint64_t palette, uint bones) {
    const uint vertex = uint(gl_VertexIndex);
    const uint history = scene_characters.characters[character].clip_history;
    if (history != NoClipHistory) {
        const uint at = 3u * (history + vertex);
        clip_history.values[at] = clip.x;
        clip_history.values[at + 1u] = clip.y;
        clip_history.values[at + 2u] = clip.w;
    }
    current_clip = clip.xyw;
    const uint last = scene_characters.characters[character].last_clip_history;
    if (last_clip_valid != 0u && last != NoClipHistory && (last & ClipSwap) != 0u) {
        const uint64_t swapped = palette + uint64_t(last & ~ClipSwap) * 16u;
        Rows rows = Rows(swapped);
        const mat4 last_matrix = mat4(rows.rows[0], rows.rows[1], rows.rows[2], rows.rows[3]);
        const vec3 last_model = SkinPosition(unskinned, skin, swapped + 64u, bones);
        last_clip = (last_matrix * vec4(last_model, 1.0)).xyw;
    } else if (last_clip_valid != 0u && last != NoClipHistory) {
        const uint at = 3u * (last + vertex);
        last_clip = vec3(last_clip_history.values[at], last_clip_history.values[at + 1u],
                         last_clip_history.values[at + 2u]);
    } else {
        last_clip = (light.view_to_previous * vec4(view, 1.0)).xyw;
    }
}
#endif

// The game's G2_CH_Main vertex programs: the view-space frame, with the normal bent toward a point
// past the material's distance from it, the decal's coordinates and the model's height for the
// reflection map.
void main() {
    Draws draws = scene_draws;
    draw_index = gl_BaseInstance;
    const uint64_t vertices = draws.draws[gl_BaseInstance].vertices;
    const uint stride = draws.draws[gl_BaseInstance].stride;
    const uint bones = draws.draws[gl_BaseInstance].bones;
    const uint64_t skin = draws.draws[gl_BaseInstance].skin;
    const uint64_t palette = draws.draws[gl_BaseInstance].palette;
    // A blanked record, or a character past the table, draws nothing.
    if (!Loadable(vertices, bones, skin, palette) ||
        draws.draws[gl_BaseInstance].material - 1u >= MaxCharacters) {
        gl_Position = vec4(0.0);
        return;
    }
    const vec3 unskinned = LoadPosition(vertices, stride, draws.draws[gl_BaseInstance].position);
    const vec3 model = bones != 0u ? SkinPosition(unskinned, skin, palette, bones) : unskinned;
    const mat4 world_view = draws.draws[gl_BaseInstance].world_view;
    view_position = (world_view * vec4(model, 1.0)).xyz;
    vec4 clip = draws.draws[gl_BaseInstance].world_view_projection * vec4(model, 1.0);
#ifdef CHARACTER_MOTION
    KeepClip(clip, view_position, draws.draws[gl_BaseInstance].material - 1u, unskinned, skin,
             palette, bones);
#endif
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;

    const vec4 transform = draws.draws[gl_BaseInstance].texcoord_transform;
    texcoord = transform.xy + transform.zw * LoadTexcoord(vertices, stride,
                                                          draws.draws[gl_BaseInstance].texcoord);
    const uint decal_index = draws.draws[gl_BaseInstance].decal;
    decal_texcoord = decal_index != 0u ? DecalTexcoord(unskinned, decal_index - 1u) : vec2(0.0);
    model_xy = unskinned.xy;

    const uint normal_element = draws.draws[gl_BaseInstance].normal;
    vec3 n = (normal_element >> 8) != 0u ? LoadNormal(vertices, stride, normal_element)
                                         : vec3(0.0, 0.0, 1.0);
    n = bones != 0u ? SkinDirection(n, skin, palette, bones) : n;
    n = mat3(world_view) * n;
    float rim = 1.0;
    const uint index = draws.draws[gl_BaseInstance].material - 1u;
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

    const uint tangent_element = draws.draws[gl_BaseInstance].tangent;
    tangent = vec4(1.0, 0.0, 0.0, 1.0);
    if ((tangent_element >> 8) != 0u) {
        const vec4 t = LoadTangent(vertices, stride, tangent_element);
        const vec3 turned = bones != 0u ? SkinDirection(t.xyz, skin, palette, bones) : t.xyz;
        tangent = vec4(normalize(mat3(world_view) * turned), -t.w);
    }
    const uint color_element = draws.draws[gl_BaseInstance].vertex_color;
    vertex_color = (color_element >> 8) != 0u ? LoadVertexColor(vertices, stride, color_element)
                                              : vec4(1.0);
}
