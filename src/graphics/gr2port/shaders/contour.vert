// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Contours scene_contours;
};

// The direction a silhouette's vertex moves outward on the screen.
layout(location = 0) out vec2 direction;
layout(location = 1) out float facing;
// A position along the line, for its waves.
layout(location = 2) out float phase;
layout(location = 3) out float strength;
layout(location = 4) flat out uint contour_index;

// The game's G2_CH_Contour vertex programs, in OpenGL clip space: the view normal's direction, and
// a strength that grows with the distance from a point of the character.
void main() {
    Contours table = scene_contours;
    contour_index = gl_DrawID;
    const uint64_t vertices = table.contours[gl_DrawID].vertices;
    const uint stride = table.contours[gl_DrawID].stride;
    const uint bones = table.contours[gl_DrawID].bones;
    const uint64_t skin = table.contours[gl_DrawID].skin;
    const uint64_t palette = table.contours[gl_DrawID].palette;
    const vec3 unskinned = LoadPosition(vertices, stride, table.contours[gl_DrawID].position);
    const vec3 model = bones != 0u ? SkinPosition(unskinned, skin, palette, bones) : unskinned;
    vec3 n = LoadNormal(vertices, stride, table.contours[gl_DrawID].normal);
    n = bones != 0u ? SkinDirection(n, skin, palette, bones) : n;
    const mat4 world_view = table.contours[gl_DrawID].world_view;
    gl_Position = table.contours[gl_DrawID].world_view_projection * vec4(model, 1.0);
    const vec3 view_position = (world_view * vec4(model, 1.0)).xyz;
    const vec3 view_normal = normalize(mat3(world_view) * n);
    direction = -view_normal.xy * inversesqrt(max(dot(view_normal.xy, view_normal.xy), 1e-12));
    facing = abs(view_normal.z);
    phase = 0.001 * (unskinned.x + unskinned.y + unskinned.z);
    const vec4 center = table.contours[gl_DrawID].fade_center;
    const vec3 offset = view_position - center.xyz;
    const float offset_length = length(offset);
    const float turned =
        max(1.0 - table.contours[gl_DrawID].fading * abs(offset.z / offset_length), 0.0);
    const float reach = min(center.w * offset_length, 1.0);
    strength = clamp(reach * (reach + turned - reach * turned) +
                         table.contours[gl_DrawID].transparency,
                     0.0, 1.0);
}
