// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(triangles_adjacency) in;
layout(triangle_strip, max_vertices = 12) out;

layout(push_constant) uniform Constants {
    Contours scene_contours;
};

layout(location = 0) in vec2 direction[];
layout(location = 1) in float facing[];
layout(location = 2) in float phase[];
layout(location = 3) in float strength[];
layout(location = 4) flat in uint contour_index[];

// x along the edge, y across the ribbon from its outer side, z the line's phase, w its strength.
layout(location = 0) out vec4 ribbon;
layout(location = 1) flat out uint ribbon_contour;

vec3 Ndc(int i) {
    return gl_in[i].gl_Position.xyz / gl_in[i].gl_Position.w;
}

void EmitRibbon(vec2 xy, float z, float w, vec4 value) {
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    gl_Position = vec4(xy, 0.5 * (w - z), w);
    ribbon = value;
    ribbon_contour = contour_index[0];
    EmitVertex();
}

// One edge of the triangle, when its neighbour across the edge turns away: a ribbon around the
// edge's two vertices, its outer side pushed back in depth so that the character covers it.
void Edge(int a, int adjacent, int b, int opposite, float turn) {
    const vec3 pa = Ndc(a);
    const vec3 pb = Ndc(b);
    const vec3 po = Ndc(opposite);
    const vec3 pn = Ndc(adjacent);
    // An edge with no neighbour repeats the opposite vertex.
    if (any(equal(pn, po))) {
        return;
    }
    const vec3 along = normalize(pb - pa);
    const vec3 to_neighbour = normalize(pn - pa);
    const float neighbour_turn = to_neighbour.x * along.y - to_neighbour.y * along.x;
    if (neighbour_turn > 0.0) {
        return;
    }
    Contours table = scene_contours;
    const uint index = contour_index[0];
    const vec2 inverse_z = table.contours[index].inverse_z;
    const float size = table.contours[index].size_amplitude;
    const vec2 bias = table.contours[index].bias;

    const vec2 to_opposite = po.xy - pa.xy;
    const float t = dot(to_opposite, along.xy);
    const vec2 perpendicular = to_opposite - along.xy * t;
    const float distance_a = 1.0 / (inverse_z.x * pa.z + inverse_z.y);
    const float distance_b = 1.0 / (inverse_z.x * pb.z + inverse_z.y);
    const float distance_o = 1.0 / (inverse_z.x * po.z + inverse_z.y);
    const float slope = (distance_b - distance_a) / length(pb.xy - pa.xy);
    const float foot = distance_a + slope * t;
    const float pushed = foot + (distance_o - foot) * size / length(perpendicular);
    const float behind = max(table.contours[index].offset, pushed - slope * t - distance_a);
    const float pushed_a = distance_a + behind;
    const float pushed_b = distance_b + behind;

    // A sharp turn shows the full line where the surface faces the eye.
    const float sharp = table.contours[index].threshold >= abs(turn - neighbour_turn) ? 1.0 : 0.0;
    const float fading = table.contours[index].fading_scale;
    const float amount_a = max(clamp(2.0 * fading * facing[a] * sharp, 0.0, 1.0) - bias.x, 0.0);
    const float amount_b = max(clamp(2.0 * fading * facing[b] * sharp, 0.0, 1.0) - bias.x, 0.0);
    const float strength_a = strength[a] * (1.0 - bias.y * amount_a);
    const float strength_b = strength[b] * (1.0 - bias.y * amount_b);

    const vec4 clip_a = gl_in[a].gl_Position;
    const vec4 clip_b = gl_in[b].gl_Position;
    const vec2 move_a = size * clip_a.w * direction[a];
    const vec2 move_b = size * clip_b.w * direction[b];
    const float depth_a = (1.0 - inverse_z.y * pushed_a) / (inverse_z.x * pushed_a);
    const float depth_b = (1.0 - inverse_z.y * pushed_b) / (inverse_z.x * pushed_b);
    EmitRibbon(clip_a.xy - move_a, clip_a.z, clip_a.w, vec4(0.0, 1.0, phase[a], strength_a));
    EmitRibbon(clip_a.xy + move_a, clip_a.w * depth_a, clip_a.w,
               vec4(0.0, 0.0, phase[a], strength_a));
    EmitRibbon(clip_b.xy - move_b, clip_b.z, clip_b.w, vec4(1.0, 1.0, phase[b], strength_b));
    EmitRibbon(clip_b.xy + move_b, clip_b.w * depth_b, clip_b.w,
               vec4(1.0, 0.0, phase[b], strength_b));
    EndPrimitive();
}

// The game's G2_CH_Contour geometry program: triangles that face the eye give the silhouette edges
// they share with triangles that do not.
void main() {
    const vec3 p0 = Ndc(0);
    const vec3 p2 = Ndc(2);
    const vec3 p4 = Ndc(4);
    const vec3 first = normalize(p2 - p0);
    const vec3 second = normalize(p4 - p2);
    const float turn = first.x * second.y - first.y * second.x;
    if (turn < 0.0) {
        return;
    }
    Edge(0, 1, 2, 4, turn);
    Edge(2, 3, 4, 0, turn);
    Edge(4, 5, 0, 2, turn);
}
