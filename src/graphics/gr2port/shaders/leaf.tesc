// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(vertices = 3) out;

layout(push_constant) uniform Constants {
    Leaves scene_leaves;
};

layout(location = 0) in vec3 control_point[];
layout(location = 1) in uint leaf_index[];
layout(location = 0) out vec3 curve_point[];
layout(location = 1) patch out uint leaf;

// The game's leaf hull program: the patch's isolines are its leaves, each of the leaf's
// tessellation segments.
void main() {
    curve_point[gl_InvocationID] = control_point[gl_InvocationID];
    if (gl_InvocationID == 0) {
        Leaves leaves = scene_leaves;
        leaf = leaf_index[0];
        gl_TessLevelOuter[0] = leaves.leaves[leaf_index[0]].isolines;
        gl_TessLevelOuter[1] = leaves.leaves[leaf_index[0]].tessellation;
    }
}
