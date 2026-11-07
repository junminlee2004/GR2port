// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "scene.glsl"
#include "surface.glsl"

layout(push_constant) uniform Constants {
    GrassInstances scene_grass;
};

// The normal's lean in xy and the texture coordinates in zw.
layout(location = 0) in vec4 surface;
// The blade's direction in xy, and its side in z.
layout(location = 1) in vec3 along;
layout(location = 2) flat in uint instance;

layout(location = 0) out vec4 g_albedo;
layout(location = 1) out vec2 g_normal;
layout(location = 2) out vec4 g_surface;

// The game's grass pixel program without its velocity: the texture over the colour from the tip to
// the root, cut out by its alpha and a screen door, with a normal map in the blade's frame. Its
// albedo alpha, which holds the world's hatching, is 1.
void main() {
    GrassInstances instances = scene_grass;
    const vec4 color = Sample(instances.instances[instance].color_map, surface.zw);
    if (ScreenDoorShut(instances.instances[instance].punch_through) ||
        color.a <= instances.instances[instance].alpha_reference) {
        discard;
    }
    const vec3 b = normalize(vec3(along.xy, sqrt(max(0.0, 1.0 - dot(along.xy, along.xy)))));
    const vec3 facing =
        normalize(vec3(surface.xy, sqrt(max(0.0, 1.0 - dot(surface.xy, surface.xy)))));
    const vec3 t = along.z * cross(facing, b);
    vec3 tangent_normal = vec3(0.0, 0.0, 1.0);
    const uint normal_map = instances.instances[instance].normal_map;
    if (normal_map != 0u) {
        const vec3 texel = 2.0 * Sample(normal_map, surface.zw).rgb - 1.0;
        tangent_normal =
            normalize(vec3(instances.instances[instance].normal_scale * texel.xy, texel.z));
    }
    const vec3 n =
        normalize(b * tangent_normal.x + t * tangent_normal.y + facing * tangent_normal.z);
    g_albedo = vec4(color.rgb * mix(instances.instances[instance].tip_color.rgb,
                                    instances.instances[instance].root_color.rgb, surface.w),
                    1.0);
    g_normal = PackNormal(n);
    g_surface = vec4(instances.instances[instance].roughness,
                     instances.instances[instance].metallic,
                     instances.instances[instance].translucency, 0.0);
}
