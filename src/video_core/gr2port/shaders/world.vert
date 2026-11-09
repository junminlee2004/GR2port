// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Materials scene_materials;
};

layout(location = 0) out vec3 normal;
// The tangent, and in w the sign of the bitangent.
layout(location = 1) out vec4 tangent;
// Texture coordinates, and in zw the parallax direction.
layout(location = 2) out vec4 texcoord;
layout(location = 3) out vec4 vertex_color;
layout(location = 4) out vec3 gradation;
layout(location = 5) flat out uint draw_index;

// The game's G2_BG_Main vertex programs without their velocity: the view-space frame, the parallax
// direction in the frame's tangent plane, and the colour the model's height blends to.
void main() {
    Draws draws = scene_draws;
    draw_index = gl_InstanceIndex;
    const uint64_t vertices = draws.draws[gl_InstanceIndex].vertices;
    const uint stride = draws.draws[gl_InstanceIndex].stride;
    const uint bones = draws.draws[gl_InstanceIndex].bones;
    const uint64_t skin = draws.draws[gl_InstanceIndex].skin;
    const uint64_t palette = draws.draws[gl_InstanceIndex].palette;
    const vec3 unskinned = LoadPosition(vertices, stride, draws.draws[gl_InstanceIndex].position);
    const vec3 model =
        bones != 0u ? SkinPosition(unskinned, skin, palette, bones) : unskinned;
    const mat4 world_view = draws.draws[gl_InstanceIndex].world_view;
    const vec3 view_position = (world_view * vec4(model, 1.0)).xyz;
    vec4 clip = draws.draws[gl_InstanceIndex].world_view_projection * vec4(model, 1.0);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;

    const vec4 transform = draws.draws[gl_InstanceIndex].texcoord_transform;
    const vec2 uv = LoadTexcoord(vertices, stride, draws.draws[gl_InstanceIndex].texcoord);
    const uint material = draws.draws[gl_InstanceIndex].material;
    texcoord = vec4(material != 0u ? uv : transform.xy + transform.zw * uv, 0.0, 0.0);

    const uint normal_element = draws.draws[gl_InstanceIndex].normal;
    const uint tangent_element = draws.draws[gl_InstanceIndex].tangent;
    normal = vec3(0.0);
    tangent = vec4(0.0);
    if ((normal_element >> 8) != 0u) {
        vec3 n = LoadNormal(vertices, stride, normal_element);
        n = bones != 0u ? SkinDirection(n, skin, palette, bones) : n;
        normal = normalize(mat3(world_view) * n);
    }
    if ((normal_element >> 8) != 0u && (tangent_element >> 8) != 0u) {
        const vec4 t = LoadTangent(vertices, stride, tangent_element);
        const vec3 turned = bones != 0u ? SkinDirection(t.xyz, skin, palette, bones) : t.xyz;
        tangent = vec4(normalize(mat3(world_view) * turned), t.w);
        const vec3 eye = normalize(view_position);
        const float facing = 1.0 / dot(normal, eye);
        texcoord.zw = 0.1 * facing *
                      vec2(dot(tangent.xyz, eye), t.w * dot(eye, cross(normal, tangent.xyz)));
    }
    const uint color_element = draws.draws[gl_InstanceIndex].vertex_color;
    vertex_color = (color_element >> 8) != 0u ? LoadVertexColor(vertices, stride, color_element)
                                              : vec4(1.0);
    gradation = vec3(1.0);
    if (material != 0u) {
        const uint index = material - 1u;
        const vec4 low = scene_materials.materials[index].gradation_low;
        const vec4 high = scene_materials.materials[index].gradation_high;
        gradation = mix(low.rgb, high.rgb, clamp(high.w * (unskinned.y - low.w), 0.0, 1.0));
        // A gem has no gradation: its pixels refract the view ray instead.
        if ((scene_materials.materials[index].flags & MaterialGem) != 0u) {
            gradation = view_position;
        }
    }
}
