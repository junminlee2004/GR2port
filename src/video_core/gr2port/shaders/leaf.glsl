// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A point of a leaf, for leaf.tese and leaf_shadow.tese: the game's leaf domain programs up to
// their projections. Each isoline of a patch is one leaf, at t = gl_TessCoord.y on the branch's
// curve; u = gl_TessCoord.x runs from the leaf's base to its tip.

#include "scene.glsl"

layout(isolines, equal_spacing) in;

layout(location = 0) in vec3 curve_point[];
layout(location = 1) patch in uint leaf;

layout(location = 0) out vec4 leaf_position;
layout(location = 1) out vec3 leaf_side;
layout(location = 2) out float leaf_along;
layout(location = 3) out uint leaf_index;

// The game's cosine takes revolutions: the fraction of the angle over 2 pi.
vec3 Cosine(vec3 angle) {
    return cos(6.2831855 * fract(abs(angle * 0.159154937)));
}

// The point u along the leaf at t, in world space, and the offset of the leaf's edge there, in
// model space. The leaf leaves the curve's point at the refraction of the curve's direction
// through a direction across it, which a direction of its own sets; its width turns along it by
// the twist, and gravity bends its heading down in world space.
void GrowLeaf(Leaves leaves, uint i, out vec4 position, out vec3 offset) {
    const float t = gl_TessCoord.y;
    const float u = gl_TessCoord.x;
    const vec3 p0 = curve_point[0];
    const vec3 a = p0 + t * (curve_point[1] - p0);
    const vec3 b = curve_point[1] + t * (curve_point[2] - curve_point[1]);
    const vec3 tangent = b - a;
    const vec3 base = a + t * tangent;
    const vec3 branch = normalize(tangent);
    const vec3 random = normalize(Cosine(p0 * (100.0 * t + 100.0)));
    const vec3 across = cross(random, branch);
    const float eta = leaves.leaves[i].direction;
    const float slant = dot(across, branch);
    const float k = eta * eta - eta * eta * slant * slant;
    const vec3 refracted =
        (k < 1.0 ? 1.0 : 0.0) * (eta * branch + (-eta * slant - sqrt(abs(1.0 - k))) * across);
    const vec3 direction = normalize(refracted);
    const vec3 width = random + leaves.leaves[i].twist * u * (across - random);
    offset = leaves.leaves[i].width * cross(cross(width, branch), direction);
    const mat4 world = leaves.leaves[i].world;
    const vec4 heading =
        world * vec4(direction, 0.0) - vec4(0.0, leaves.leaves[i].gravity * u, 0.0, 0.0);
    position = world * vec4(base, 1.0) + leaves.leaves[i].leaf_length * u * normalize(heading);
}
