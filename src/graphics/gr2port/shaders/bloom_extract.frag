// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(push_constant) uniform Constants {
    Grade grade;
    // One pixel of the scene in texture coordinates.
    vec2 scene_pixel;
};

layout(location = 0) in vec2 position;

layout(location = 0) out vec4 color;

// The share past a soft edge, by the game's smooth curve.
float SoftStep(float start, float end, float value) {
    const float t = clamp((value - start) / (end - start), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// The game's bloom bright pass (PS 0x1bb9644) into a quarter-size image: the brightest of four
// samples one scene pixel away, kept by its own level and by its luma, raised to a power and
// scaled by a weight of depth. Alpha keeps the depth term for the blur's width.
void main() {
    const vec2 uv = 0.5 * position + 0.5;
    // Reverse depth to the game's depth, 1 far.
    const float depth = pow(abs(1.0 - texture(scene_depth, uv).r), grade.bloom_correction.y);
    const vec3 c = max(max(texture(scene, uv + scene_pixel).rgb,
                           texture(scene, uv + vec2(-scene_pixel.x, scene_pixel.y)).rgb),
                       max(texture(scene, uv + vec2(scene_pixel.x, -scene_pixel.y)).rgb,
                           texture(scene, uv - scene_pixel).rgb));
    const float luma = dot(c, vec3(0.298912, 0.586611, 0.114478));
    const float gate = SoftStep(grade.bloom_low.w, grade.bloom_high.w, luma);
    const vec3 kept = c *
                      vec3(SoftStep(grade.bloom_low.r, grade.bloom_high.r, c.r),
                           SoftStep(grade.bloom_low.g, grade.bloom_high.g, c.g),
                           SoftStep(grade.bloom_low.b, grade.bloom_high.b, c.b)) *
                      gate;
    const vec4 depths = grade.bloom_depths;
    const float scale =
        grade.bloom_correction.z * (1.0 - SoftStep(depths.x, depths.y, depth)) +
        grade.bloom_correction.w * SoftStep(depths.z, depths.w, depth);
    color = vec4((1.0 + scale) * pow(abs(kept), vec3(grade.bloom_correction.x)), depth);
}
