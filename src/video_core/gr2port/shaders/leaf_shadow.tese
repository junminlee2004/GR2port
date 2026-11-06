// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "leaf.glsl"

layout(push_constant) uniform Constants {
    Leaves scene_leaves;
    Shadows shadows;
    uint cascade;
};

// A leaf's point in a cascade: the cascade's plain view-projection after the leaf's world matrix,
// as the game's callback sets the leaf shadow's constants.
void main() {
    Leaves leaves = scene_leaves;
    vec4 position;
    vec3 offset;
    GrowLeaf(leaves, leaf, position, offset);
    const mat4 view_projection = shadows.view_projection[cascade * CasterTables];
    leaf_position = view_projection * position;
    leaf_side = (view_projection * (leaves.leaves[leaf].world * vec4(offset, 0.0))).xyz;
    leaf_along = 1.0 - gl_TessCoord.x;
    leaf_index = leaf;
}
