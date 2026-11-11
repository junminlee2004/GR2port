// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// The fog's cells: columns over the screen, each of slices that grow with depth, the game's 160 by
// 90 by 128 unless the fog quality sets fewer. A slice's depth runs as the square of its share,
// between the near plane and the fog's far one.

layout(constant_id = 0) const int FogColumns = 160;
layout(constant_id = 1) const int FogRows = 90;
layout(constant_id = 2) const int FogSlices = 128;

// The game's jitter of a cell from 0 to 1, by its place and a clock.
float FogJitter(ivec3 cell, float clock) {
    const float share = float(cell.z) / float(FogSlices);
    const float seed = 256.0 * (clock * uintBitsToFloat(0x41ce725au) +
                                uintBitsToFloat(0x3e1419b6u) * (float(cell.x) * share) +
                                uintBitsToFloat(0x3cf2965du) * (float(cell.y) * share)) +
                       uintBitsToFloat(0x33d6bf95u);
    return fract(fract(abs(uintBitsToFloat(0x4ceb79a3u) / seed)) * abs(seed));
}

// A cell's corner in normalized device coordinates, moved by a jitter in pixels of the grid.
vec2 FogNdc(ivec3 cell, float shift) {
    return vec2((2.0 * float(cell.x) + shift) / float(FogColumns) - 1.0,
                1.0 - (2.0 * float(cell.y) - shift) / float(FogRows));
}

// The view position at ndc and depth share t; projection is (1 / P00, 1 / P11, near - far, -near).
vec3 FogPosition(vec2 ndc, float t, vec4 projection) {
    const float z = projection.z * t + projection.w;
    return vec3(-z * ndc.x * projection.x, -z * ndc.y * projection.y, z);
}

// Where a view position was among the last frame's cells, in texture coordinates.
vec3 FogLastCell(vec3 position, mat4 view_to_previous, vec4 projection) {
    const vec4 clip = view_to_previous * vec4(position, 1.0);
    const vec2 ndc = clip.xy / clip.w;
    const float t = (-clip.w - projection.w) / projection.z;
    return vec3(clamp(0.5 * ndc.x + 0.5, 0.0, 1.0), clamp(0.5 - 0.5 * ndc.y, 0.0, 1.0),
                sqrt(clamp(t, 0.0, 1.0)));
}
