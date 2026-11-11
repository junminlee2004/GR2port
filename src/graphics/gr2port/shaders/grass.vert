// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(push_constant) uniform Constants {
    GrassInstances scene_grass;
    Blades scene_blades;
};

// The normal's lean in xy and the texture coordinates in zw.
layout(location = 0) out vec4 surface;
// The blade's direction in xy, and its side in z.
layout(location = 1) out vec3 along;
layout(location = 2) flat out uint instance;

// The game's grass geometry program without its velocity, one corner of a triangle a blade: two at
// the root, as far apart as its fade sets, and the tip. Each end's normal faces the eye.
void main() {
    const uint index = gl_VertexIndex / 3u;
    const uint corner = gl_VertexIndex % 3u;
    surface = vec4(0.0);
    along = vec3(0.0);
    instance = 0u;
    // The kernel counts past the buffer when it fills.
    if (index >= MaxBlades) {
        gl_Position = vec4(0.0);
        return;
    }
    Blades blades = scene_blades;
    GrassInstances instances = scene_grass;
    const vec3 root = blades.blades[index].root;
    const vec3 tip = blades.blades[index].tip;
    instance = blades.blades[index].instance;
    const mat4 world_view_projection = instances.instances[instance].draw_world_view_projection;
    const mat3 turn = mat3(world_view_projection);
    const vec3 direction = normalize(tip - root);
    const vec3 point = corner == 2u ? tip : root;
    const vec3 view = normalize(point - instances.instances[instance].eye.xyz);
    vec4 clip = world_view_projection * vec4(point, 1.0);
    float side = 0.0;
    if (corner != 2u) {
        side = corner == 0u ? -1.0 : 1.0;
        const float width =
            0.03 + blades.blades[index].fade * (instances.instances[instance].width - 0.03);
        clip.xy += side * (turn * (normalize(cross(view, direction)) * width)).xy;
    }
    surface = vec4(-(turn * view).xy, corner == 2u ? 0.5 : float(corner), corner == 2u ? 0.0 : 1.0);
    along = vec3((turn * direction).xy, side);
    // OpenGL depth, -w near to w far, to reverse depth, 1 near to 0 far.
    clip.z = 0.5 * (clip.w - clip.z);
    gl_Position = clip;
}
