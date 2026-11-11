// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Leaves scene_leaves;
};

layout(location = 0) out vec3 control_point;
layout(location = 1) out uint leaf_index;

// The game's leaf vertex program: a control point of a branch's curve, for the leaves of the leaf
// at gl_BaseInstance.
void main() {
    Leaves leaves = scene_leaves;
    leaf_index = gl_BaseInstance;
    control_point = LoadPosition(leaves.leaves[gl_BaseInstance].vertices,
                                 leaves.leaves[gl_BaseInstance].stride,
                                 leaves.leaves[gl_BaseInstance].position);
}
