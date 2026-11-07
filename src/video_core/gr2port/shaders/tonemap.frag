// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D bloom;

layout(push_constant) uniform Constants {
    Grade grade;
    Meter meter;
};

layout(location = 0) in vec2 position;

layout(location = 0) out vec4 color;

// The game's Tonemap program: a filmic curve scaled so a white of 11.2 maps to 1, then the bloom.
// Alpha holds the luma that the antialias program reads.
void main() {
    const vec2 texcoord = 0.5 * position + 0.5;
    const vec3 x = max(texture(scene, texcoord).rgb, vec3(0.0)) * meter.exposure;
    const vec3 curve = ((0.22 * x + 0.03) * x + 0.002) / ((0.22 * x + 0.3) * x + 0.06);
    vec3 toned = 2.3060 * curve - 0.07687;
    if (grade.bloom_strength.w != 0.0) {
        toned += grade.bloom_strength.rgb * texture(bloom, texcoord).rgb;
    }
    color = vec4(toned, dot(toned, vec3(0.299, 0.587, 0.114)));
}
