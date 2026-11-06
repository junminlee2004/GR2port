// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "scene.glsl"
#include "surface.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(push_constant) uniform Constants {
    Leaves scene_leaves;
};

// The normal's lean in xy and the texture coordinates in zw.
layout(location = 0) in vec4 surface;
layout(location = 1) flat in uint leaf;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec2 g_normal;
layout(location = 2) out vec4 g_surface;

// The game's leaf pixel program: the leaf texture over the colour from the tip to the base, cut
// out by its alpha and a screen door, with a normal that faces the eye and leans as the geometry
// program says. Its albedo alpha, which holds the world's hatching, is 1.
void main() {
    Leaves leaves = scene_leaves;
    const vec4 color =
        texture(sampler2D(textures[nonuniformEXT(leaves.leaves[leaf].texture)], texture_sampler),
                surface.zw);
    if (ScreenDoorShut(leaves.leaves[leaf].punch_through) ||
        color.a <= leaves.leaves[leaf].alpha_reference) {
        discard;
    }
    const vec3 tint =
        mix(leaves.leaves[leaf].tip_color.rgb, leaves.leaves[leaf].root_color.rgb, surface.w);
    const vec2 lean = surface.xy;
    const vec3 n = normalize(vec3(lean, sqrt(max(0.0, 1.0 - dot(lean, lean)))));
    g_albedo = vec4(color.rgb * tint, 1.0);
    g_normal = PackNormal(n);
    g_surface = vec4(leaves.leaves[leaf].roughness, leaves.leaves[leaf].metallic,
                     leaves.leaves[leaf].translucency, 0.0);
}
