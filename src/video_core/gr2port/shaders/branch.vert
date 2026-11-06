// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(push_constant) uniform Constants {
    Branches scene_branches;
};

// The normal's lean in xy and the texture coordinates in zw.
layout(location = 0) out vec4 surface;
layout(location = 1) out vec3 color;
layout(location = 2) flat out uint branch;

// The two triangles of a quad between two points of a curve, each corner an end and a side.
const uint QuadCorners[6] = uint[6](0u, 1u, 2u, 2u, 1u, 3u);

vec3 Point(uint64_t vertices, uint point, uint stride) {
    Words words = Words(vertices + uint64_t(point) * stride);
    return uintBitsToFloat(uvec3(words.words[0], words.words[1], words.words[2]));
}

// The game's branch domain and geometry programs without their velocity: a ribbon along each
// quadratic Bezier curve of three vertices, of the instance's segments, facing the eye. A vertex is
// a quad's corner, of a segment, of a curve.
void main() {
    Branches branches = scene_branches;
    branch = gl_InstanceIndex;
    const uint corner = QuadCorners[gl_VertexIndex % 6u];
    const uint quad = gl_VertexIndex / 6u;
    const uint segments = branches.branches[branch].segments;
    const uint first_point = quad / segments * 3u;
    const uint side_index = corner & 1u;
    const float u = float(quad % segments + (corner >> 1u)) / float(segments);
    const uint64_t vertices = branches.branches[branch].vertices;
    const uint stride = branches.branches[branch].stride;
    const vec3 p0 = Point(vertices, first_point, stride);
    const vec3 p1 = Point(vertices, first_point + 1u, stride);
    const vec3 p2 = Point(vertices, first_point + 2u, stride);
    const vec3 a = mix(p0, p1, u);
    const vec3 b = mix(p1, p2, u);
    const vec3 point = mix(a, b, u);
    const vec3 view = normalize(point - branches.branches[branch].eye.xyz);
    const float width = mix(branches.branches[branch].root_width,
                            branches.branches[branch].tip_width, u);
    const vec3 across = normalize(cross(view, normalize(b - a))) * width;
    const mat4 world_view_projection = branches.branches[branch].world_view_projection;
    const vec2 clip_across = (mat3(world_view_projection) * across).xy;
    const float side = side_index == 0u ? -1.0 : 1.0;
    vec4 clip = world_view_projection * vec4(point, 1.0);
    clip.xy += side * clip_across;
    const vec4 uv_range = branches.branches[branch].uv_range;
    surface = vec4(side * normalize(clip_across), uv_range.x + float(side_index) * uv_range.z,
                   uv_range.y + uv_range.w * (1.0 - u));
    color = mix(branches.branches[branch].root_color.rgb, branches.branches[branch].tip_color.rgb,
                u);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
}
