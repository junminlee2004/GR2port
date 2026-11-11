// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(push_constant) uniform Constants {
    Contours scene_contours;
};

layout(location = 0) in vec4 ribbon;
layout(location = 1) flat in uint ribbon_contour;

layout(location = 0) out vec4 color;

// The game's G2_CH_Contour pixel program: a line across the ribbon, its width and centre waving
// along it like a hand-drawn stroke, with soft edges.
void main() {
    Contours table = scene_contours;
    const uint index = ribbon_contour;
    const float width = table.contours[index].size;
    const float amplitude = table.contours[index].line_amplitude;
    const float curve_amplitude = table.contours[index].curve_amplitude;
    const float curve_cycle = table.contours[index].curve_cycle;
    const float along = ribbon.z;
    const float curve = curve_amplitude / (curve_amplitude + 0.5 * width) *
                        (cos(curve_cycle * (30000.0 * along + 9000.0)) +
                         cos(curve_cycle * along * 100000.0));
    const float across = 2.0 * ribbon.y - 1.0;
    const float wave = cos(table.contours[index].line_cycle * along * 160000.0);
    const float stroke =
        across + 0.5 * amplitude * (across - 0.5 * curve) * (1.0 + wave);
    const float full_width = width / (width + 2.0 * curve_amplitude);
    const float from_center = abs(0.5 * curve - stroke) * 2.0 / full_width;
    const float softness = table.contours[index].softness;
    const float alpha = max(table.contours[index].color.a * ribbon.w *
                                min((1.0 - from_center) / softness, 1.0),
                            0.0);
    if (alpha < 0.004) {
        discard;
    }
    color = vec4(table.contours[index].color.rgb, alpha);
}
