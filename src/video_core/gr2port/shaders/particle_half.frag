// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

// The scene's depth, or the half target's colour.
layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Constants {
    uint composite;
};

layout(location = 0) out vec4 color;

// A half pixel's depth: the farthest of the scene's 2x2 pixels under it, the least in reverse
// depth, so that a particle in front of any of them draws.
void Downsample() {
    const ivec2 last = textureSize(source, 0) - 1;
    const ivec2 corner = 2 * ivec2(gl_FragCoord.xy);
    const float a = texelFetch(source, min(corner, last), 0).r;
    const float b = texelFetch(source, min(corner + ivec2(1, 0), last), 0).r;
    const float c = texelFetch(source, min(corner + ivec2(0, 1), last), 0).r;
    const float d = texelFetch(source, min(corner + ivec2(1, 1), last), 0).r;
    gl_FragDepth = min(min(a, b), min(c, d));
}

// The half target's premultiplied colour, with the share of the scene that shows through in
// alpha, filtered by hand from the four texels around the pixel. Where all four still hold the
// clear value, the pixel keeps the scene's colour and alpha.
void Composite() {
    const ivec2 last = textureSize(source, 0) - 1;
    const vec2 at = 0.5 * gl_FragCoord.xy - 0.5;
    const ivec2 base = ivec2(floor(at));
    const vec2 weight = at - vec2(base);
    const vec4 c00 = texelFetch(source, clamp(base, ivec2(0), last), 0);
    const vec4 c10 = texelFetch(source, clamp(base + ivec2(1, 0), ivec2(0), last), 0);
    const vec4 c01 = texelFetch(source, clamp(base + ivec2(0, 1), ivec2(0), last), 0);
    const vec4 c11 = texelFetch(source, clamp(base + ivec2(1, 1), ivec2(0), last), 0);
    const vec4 empty = vec4(0.0, 0.0, 0.0, 1.0);
    if (c00 == empty && c10 == empty && c01 == empty && c11 == empty) {
        discard;
    }
    color = mix(mix(c00, c10, weight.x), mix(c01, c11, weight.x), weight.y);
}

void main() {
    if (composite != 0u) {
        Composite();
    } else {
        Downsample();
    }
}
