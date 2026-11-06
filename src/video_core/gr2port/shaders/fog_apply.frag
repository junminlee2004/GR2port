// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(set = 0, binding = 1) uniform sampler3D scattered;

layout(push_constant) uniform Constants {
    Fog fog;
};

layout(location = 0) out vec4 color;

// The game's FogRender (CS 0x1c2b88c) as a blend over the scene: the scattered light at the
// pixel's depth, a fog near the ground below a height, and a fade toward the fog's colour as less
// of the scene shows. The blend adds color.rgb to the scene times 1 - color.a.
void main() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy);
    const vec2 size = vec2(textureSize(scene_depth, 0));
    // Reverse depth to the game's depth.
    const float z = 1.0 - texelFetch(scene_depth, pixel, 0).r;
    const float distance_to_eye = 1.0 / (fog.apply_depth.x * (2.0 * z - 1.0) + fog.apply_depth.y);
    const vec2 uv = (vec2(pixel) + 0.5) / size;
    const vec2 ndc = vec2(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y);
    const vec3 position =
        (fog.view_to_rotated *
         vec4(distance_to_eye * ndc * fog.projection.xy, -distance_to_eye, 1.0))
            .xyz;
    const float low = min(fog.eye.y, position.y);
    const float high = max(fog.eye.y, position.y);
    const float below = clamp((fog.puddle_height - low) / (high - low), 0.0, 1.0);
    const float puddle = exp(-fog.puddle_density *
                             min(fog.puddle_range, length(fog.eye.xyz - position) * below));
    const float slice =
        sqrt(clamp((distance_to_eye - fog.apply_depth.z) * fog.apply_depth.w, 0.0, 1.0));
    const vec4 fogged = textureLod(scattered, vec3(uv, slice), 0.0);
    const float shown = min(fogged.a, puddle);
    const float fade = fog.apply_color.w * clamp(-log(shown), 0.0, 1.0);
    color = vec4(mix(fogged.rgb, fog.apply_color.rgb, fade), 1.0 - (1.0 - fade) * shown);
}
