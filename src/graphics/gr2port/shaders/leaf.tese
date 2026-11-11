// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "leaf.glsl"

layout(push_constant) uniform Constants {
    Leaves scene_leaves;
};

// A leaf's point in the game's clip space by its view-projection, and its edge's offset by its
// WVP.
void main() {
    Leaves leaves = scene_leaves;
    vec4 position;
    vec3 offset;
    GrowLeaf(leaves, leaf, position, offset);
    leaf_position = leaves.leaves[leaf].view_projection * position;
    leaf_side = (leaves.leaves[leaf].world_view_projection * vec4(offset, 0.0)).xyz;
    leaf_along = 1.0 - gl_TessCoord.x;
    leaf_index = leaf;
}
