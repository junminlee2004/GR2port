// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(push_constant) uniform Constants {
    WorldDecals scene_decals;
};

layout(location = 0) flat out uint decal;

// The game's decal box: x and y from -1 to 1, z from 0 to 1.
const vec3 BoxPoints[8] = vec3[8](vec3(-1.0, -1.0, 1.0), vec3(1.0, -1.0, 1.0), vec3(1.0, -1.0, 0.0),
                                  vec3(-1.0, -1.0, 0.0), vec3(-1.0, 1.0, 1.0), vec3(1.0, 1.0, 1.0),
                                  vec3(1.0, 1.0, 0.0), vec3(-1.0, 1.0, 0.0));
const uint BoxCorners[36] =
    uint[36](2u, 1u, 0u, 0u, 3u, 2u, 4u, 5u, 6u, 6u, 7u, 4u, 5u, 1u, 6u, 1u, 2u, 6u, 4u, 7u, 0u, 7u,
             3u, 0u, 7u, 6u, 3u, 6u, 2u, 3u, 5u, 4u, 1u, 4u, 0u, 1u);

// The game's decal vertex program: a corner of the decal's box.
void main() {
    WorldDecals decals = scene_decals;
    decal = gl_InstanceIndex;
    vec4 clip = decals.decals[gl_InstanceIndex].world_view_projection *
                vec4(BoxPoints[BoxCorners[gl_VertexIndex]], 1.0);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
}
