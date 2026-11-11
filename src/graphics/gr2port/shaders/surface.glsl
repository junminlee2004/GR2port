// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// What the fragment shaders of the world's surfaces share.

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

vec4 Sample(uint handle, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), uv);
}

vec2 Transformed(vec4 transform, vec2 uv) {
    return transform.xy + transform.zw * uv;
}

// The game's packed normal: the direction of xy, with the length sqrt(0.5 - 0.5 z).
vec2 PackNormal(vec3 n) {
    return n.xy * inversesqrt(max(dot(n.xy, n.xy), 1e-6)) * sqrt(abs(0.5 - 0.5 * n.z));
}

// A screen door fades a surface in. The game's depth runs the other way from this one.
bool ScreenDoorShut(float threshold) {
    const float seed = 256.0 * (0.0771356 * gl_FragCoord.x + 0.00888381 * gl_FragCoord.y) *
                           (1.0 - gl_FragCoord.z) +
                       1e-7;
    return threshold < fract(fract(abs(123456792.0 / seed)) * abs(seed));
}

// Parallax occlusion mapping through the normal map's alpha, in the normal map's coordinates: 8 to
// 14 steps along the direction by how far the surface turns from the eye, then the intersection of
// the last two steps' lines.
vec2 Parallax(uint handle, vec2 start, vec2 direction, float facing) {
    const int steps = int(14.0 - 6.0 * facing);
    const float step_size = 1.0 / float(steps);
    const vec2 dx = dFdx(start);
    const vec2 dy = dFdy(start);
    vec2 uv = start;
    float level = 1.0;
    float height = 0.0;
    float last_height = 1.0;
    for (int i = 0; i < steps; i++) {
        uv -= direction * step_size;
        height =
            textureGrad(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), uv, dx, dy).a;
        level -= step_size;
        if (height > level) {
            break;
        }
        last_height = height;
    }
    const float last_level = level + step_size;
    const float last_gap = last_level - last_height;
    const float gap = level - height;
    const float hit =
        last_gap != gap ? (level * last_gap - last_level * gap) / (last_gap - gap) : 0.0;
    return start - direction * (1.0 - hit);
}
