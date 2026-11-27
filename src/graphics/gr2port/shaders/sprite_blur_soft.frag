// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// The game's particle pixel program 0x1bcbe04, a GPU particle's blur sprite: the mean of the
// scene's red, green and blue over the material's count of taps, each tap a step further along
// the distortion map's direction, dissolved, faded and coloured as sprite_distortion_soft.frag.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "particle_gpu_push.glsl"
#include "particle_distortion.glsl"
#include "particle_gpu_material.glsl"

// The steps of red, green and blue a tap and the particle's alpha; its distance in x and the scale
// of its dissolve's place in w; the map's texture coordinates in x and y, and a vector whose
// length can pick the place on its curve in z and w; a value in thousandths that can pick it in z;
// the place on the material's curves in w; the particle's curve, material set and material, as
// bits.
layout(location = 0) in vec4 attr0;
layout(location = 1) in vec4 attr1;
layout(location = 2) in vec4 attr2;
layout(location = 3) in vec4 attr3;
layout(location = 6) in vec4 attr4;
layout(location = 10) flat in vec4 attr5;
layout(location = 0) out vec4 color0;

void main() {
    ParticleDraw draw = Draw();
    const vec2 position = GamePosition(draw);
    const uint material = GpuMaterial(draw, floatBitsToUint(attr5.y), floatBitsToUint(attr5.z));
    const uint curve_offset = floatBitsToUint(attr5.x);

    // The material's blur, or its curve: the count of taps in x, 10 below 2^-23, and from 1 in y
    // the taps step along the map's direction; below 1 every tap reads the pixel's own place.
    const vec4 blur = MaterialCurve(material, MaterialBlurCurve, curve_offset, attr4.w,
                                    MaterialVector(material, MaterialBlur));
    // At most 64 taps: a count past them comes from a broken record.
    const uint taps = blur.x < uintBitsToFloat(0x34000000u) ? 10u : min(GameUint(blur.x), 64u);
    const vec2 map_direction = DistortionDirection(DistortionMap(draw, attr2.xy));
    const vec2 direction = blur.y >= 1.0 ? map_direction : vec2(0.0);
    const vec2 inverse_size =
        vec2(1.0 / draw.constants[ScreenSize], 1.0 / draw.constants[ScreenSize + 1u]);
    vec3 sum = vec3(0.0);
    for (uint tap = 0u; tap < taps; tap++) {
        // Each tap's steps are products of their own, rounded before the direction scales them.
        precise vec3 steps = attr0.xyz * float(tap);
        sum += vec3(SceneTap(position, direction, steps.r, inverse_size).r,
                    SceneTap(position, direction, steps.g, inverse_size).g,
                    SceneTap(position, direction, steps.b, inverse_size).b);
    }
    const vec3 color = (1.0 / float(taps)) * sum;

    const float fade_scale = FadeScale(draw, material, curve_offset, attr4.w);
    const vec2 threshold = DissolveThreshold(material, curve_offset, attr4.w);
    const float particle_alpha = ErodedAlpha(draw, material, curve_offset, attr4.w, attr0.w,
                                             attr2.xy, attr1.w, attr1.x, threshold.x);
    if (uintBitsToFloat(0x34000000u) > particle_alpha) {
        discard;
    }
    const float fade =
        clamp(DepthDistance(draw, position * inverse_size, attr1.x) * fade_scale, 0.0, 1.0);
    color0 = MaterialColor(draw, color, particle_alpha, fade, threshold, attr2.zw, attr3.z,
                           curve_offset);
}
