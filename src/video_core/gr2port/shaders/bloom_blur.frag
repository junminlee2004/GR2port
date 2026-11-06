// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D last_bloom;

layout(push_constant) uniform Constants {
    Grade grade;
    // One texel of the source along the blur's direction.
    vec2 step_size;
    // 1 when the last frame's bloom blends in.
    uint feedback;
};

layout(location = 0) in vec2 position;

layout(location = 0) out vec4 color;

// The game's weight table has 16 rows of 16 Gaussian weights, row j of variance 1.875 (j + 1),
// each row scaled so that its 32 taps sum to 1.
const uint Weights = 16u;
const float VariancePerRow = 1.875;
// The last frame's bloom fades by this much each frame.
const float FeedbackFade = 1.0 / 255.0;

void RowWeights(uint row, out float weights[16]) {
    const float variance = VariancePerRow * float(row + 1u);
    float sum = 0.0;
    for (uint i = 0u; i < Weights; i++) {
        weights[i] = exp(-float(i * i) / (2.0 * variance));
        sum += 2.0 * weights[i];
    }
    for (uint i = 0u; i < Weights; i++) {
        weights[i] /= sum;
    }
}

// The game's bloom blur (PS 0x1bb84d0, and 0x1bb8d3c with the last frame): 32 taps along one axis,
// half a texel off centre, with a width picked by the depth term in alpha.
void main() {
    const vec2 uv = 0.5 * position + 0.5;
    const vec4 center = texture(source, uv);
    const float t = clamp((center.a - grade.bloom_width.x) /
                              (grade.bloom_width.y - grade.bloom_width.x),
                          0.0, 1.0);
    // The table's row as the game's bilinear sample picks it, between texel centres.
    const float row = clamp(t * t * (3.0 - 2.0 * t) * float(Weights) - 0.5, 0.0, 15.0);
    const uint row0 = uint(row);
    float weights0[16];
    float weights1[16];
    RowWeights(row0, weights0);
    RowWeights(min(row0 + 1u, Weights - 1u), weights1);
    const float between = row - float(row0);
    vec3 sum = vec3(0.0);
    for (uint i = 0u; i < Weights; i++) {
        const vec3 pair = (i == 0u ? center.rgb : texture(source, uv - float(i) * step_size).rgb) +
                          texture(source, uv + float(i + 1u) * step_size).rgb;
        sum += mix(weights0[i], weights1[i], between) * pair;
    }
    if (feedback != 0u) {
        const float keep = grade.bloom_feedback;
        sum = (1.0 - keep) * sum + keep * texture(last_bloom, uv).rgb - FeedbackFade;
    }
    for (int c = 0; c < 3; c++) {
        if (isnan(sum[c]) || isinf(sum[c])) {
            sum[c] = 0.0;
        }
    }
    color = vec4(sum, center.a);
}
