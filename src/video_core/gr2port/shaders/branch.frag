// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "scene.glsl"
#include "surface.glsl"

layout(push_constant) uniform Constants {
    Branches scene_branches;
};

// The normal's lean in xy and the texture coordinates in zw.
layout(location = 0) in vec4 surface;
layout(location = 1) in vec3 color;
layout(location = 2) flat in uint branch;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec2 g_normal;
layout(location = 2) out vec4 g_surface;

// The game's branch pixel program without its velocity: the texture over the ribbon's colour, cut
// out by its alpha and a screen door, with a normal that faces the eye and leans across the ribbon.
// Its albedo alpha, which holds the world's hatching, is 1.
void main() {
    Branches branches = scene_branches;
    const vec4 texel = Sample(branches.branches[branch].texture, surface.zw);
    if (ScreenDoorShut(branches.branches[branch].punch_through) ||
        texel.a <= branches.branches[branch].alpha_reference) {
        discard;
    }
    const vec2 lean = surface.xy;
    g_albedo = vec4(texel.rgb * color, 1.0);
    g_normal = PackNormal(normalize(vec3(lean, sqrt(max(0.0, 1.0 - dot(lean, lean))))));
    g_surface = vec4(branches.branches[branch].roughness, branches.branches[branch].metallic,
                     branches.branches[branch].translucency, 0.0);
}
