// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// A segment of a leaf, for leaf.geom and, with SHADOW, leaf_shadow.geom.

#include "scene.glsl"

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

#ifdef SHADOW
layout(push_constant) uniform Constants {
    Leaves scene_leaves;
};
#endif

layout(location = 0) in vec4 leaf_position[];
layout(location = 1) in vec3 leaf_side[];
layout(location = 2) in float leaf_along[];
layout(location = 3) in uint leaf_index[];

#ifdef SHADOW
layout(location = 0) out vec2 texcoord;
layout(location = 1) flat out uint texture_index;
layout(location = 2) flat out float alpha_reference;
#else
// The normal's lean in xy and the texture coordinates in zw.
layout(location = 0) out vec4 surface;
layout(location = 1) flat out uint leaf;
#endif

// The game's leaf geometry programs: a quad across the segment, each end pushed out to either
// side by the leaf's width in clip space, u across it and v from the tip. The colour pass leans
// each end's normal along the cross of the segment and the width.
void main() {
#ifdef SHADOW
    Leaves leaves = scene_leaves;
#endif
    const vec3 segment = leaf_position[1].xyz - leaf_position[0].xyz;
    for (int e = 0; e < 2; e++) {
#ifndef SHADOW
        const vec2 lean = normalize(cross(segment, leaf_side[e])).xy;
#endif
        for (int s = 0; s < 2; s++) {
            vec4 clip = leaf_position[e];
            clip.xy += (s == 0 ? -1.0 : 1.0) * leaf_side[e].xy;
#ifdef SHADOW
            // OpenGL depth, -w to w, to Vulkan depth, 0 to w.
            clip.z = 0.5 * (clip.z + clip.w);
            texcoord = vec2(float(s), leaf_along[e]);
            texture_index = leaves.leaves[leaf_index[e]].texture;
            alpha_reference = leaves.leaves[leaf_index[e]].alpha_reference;
#else
            // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
            clip.z = 0.5 * (clip.w - clip.z);
            surface = vec4(lean, float(s), leaf_along[e]);
            leaf = leaf_index[e];
#endif
            gl_Position = clip;
            EmitVertex();
        }
    }
    EndPrimitive();
}
