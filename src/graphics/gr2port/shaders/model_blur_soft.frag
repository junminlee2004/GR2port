// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// The game's particle pixel program 0x1be6898, a GPU particle's blur model: the mean of the
// scene's red, green and blue over the material's count of taps, each tap a step further along
// the distortion map's direction in the model's frame, or along its normal, dissolved, faded and
// coloured as model_distortion_soft.frag.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "particle_gpu_push.glsl"
#include "particle_frame.glsl"
#include "particle_gpu_material.glsl"
#include "particle_gpu_model.glsl"

// The steps of red, green and blue a tap and the particle's alpha; its distance in x and the scale
// of its dissolve's place in w; the map's texture coordinates in x and y, and a vector whose
// length can pick the place on its curve in z and w; a value in thousandths that can pick it in z;
// the frame's normal, and the place on the material's curves in w; the frame's tangent; the
// particle's curve, material set and material, as bits.
layout(location = 0) in vec4 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 2) in vec4 attr2;
layout(location = 3) in vec4 attr3;
layout(location = 6) in vec4 attr4;
layout(location = 7) in vec4 attr5;
layout(location = 10) flat in vec4 attr6;
layout(location = 0) out vec4 color0;

void main() {
    ParticleDraw draw = Draw();
    const vec2 position = GamePosition(draw);
    const uint material = GpuMaterial(draw, floatBitsToUint(attr6.y), floatBitsToUint(attr6.z));
    const uint curve_offset = floatBitsToUint(attr6.x);

    // The material's blur, or its curve: the count of taps in x, 10 below 2^-23, and from 1 in y
    // the taps step along the map's direction; below 1 they step along the normal.
    const vec4 blur = MaterialCurve(material, MaterialBlurCurve, curve_offset, attr4.w,
                                    MaterialVector(material, MaterialBlur));
    // At most 64 taps: a count past them comes from a broken record.
    const uint taps = blur.x < uintBitsToFloat(0x34000000u) ? 10u : min(GameUint(blur.x), 64u);
    const vec3 map_direction = FrameDirection(DistortionMap(draw, attr2.xy));
    const vec3 direction = blur.y >= 1.0 ? map_direction : vec3(0.0, 0.0, 1.0);
    const vec3 normal = attr4.xyz;
    const vec3 tangent = attr5.xyz;
    // The normal crossed with the tangent, x and y, each one fused step as the game's program
    // rounds them.
    const vec2 bitangent = vec2(fma(tangent.y, -normal.z, normal.y * tangent.z),
                                fma(normal.z, tangent.x, tangent.z * -normal.x));
    const vec2 inverse_size =
        vec2(1.0 / draw.constants[ScreenSize], 1.0 / draw.constants[ScreenSize + 1u]);
    vec3 sum = vec3(0.0);
    for (uint tap = 0u; tap < taps; tap++) {
        const vec3 steps = attr0.xyz * float(tap);
        // Each move is a product of its own, rounded before the frame scales it.
        precise vec3 red_move = direction * steps.r;
        precise vec3 green_move = direction * steps.g;
        precise vec3 blue_move = direction * steps.b;
        sum += vec3(
            FrameMoveTap(position, red_move, normal, tangent, bitangent, inverse_size).r,
            FrameMoveTap(position, green_move, normal, tangent, bitangent, inverse_size).g,
            FrameMoveTap(position, blue_move, normal, tangent, bitangent, inverse_size).b);
    }
    const vec3 color = (1.0 / float(taps)) * sum;

    const float fade_scale = FadeScale(draw, material, curve_offset, attr4.w);
    const vec2 threshold = DissolveThreshold(material, curve_offset, attr4.w);
    const float particle_alpha = ErodedAlpha(draw, material, curve_offset, attr4.w, attr0.w,
                                             attr2.xy, attr1.w, attr1.x, threshold.x);
    if (uintBitsToFloat(0x34000000u) > particle_alpha) {
        discard;
    }
    const float edge_fade = EdgeFade(material, curve_offset, attr4.w, attr4.z);
    if (uintBitsToFloat(0x34000000u) > edge_fade) {
        discard;
    }
    const float fade =
        clamp(DepthDistance(draw, position * inverse_size, attr1.x) * fade_scale, 0.0, 1.0);
    color0 = MaterialColor(draw, color, edge_fade * particle_alpha, fade, threshold, attr2.zw,
                           attr3.z, curve_offset);
}
