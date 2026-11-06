// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The effector spheres of the scene packet, laid out as Scene::Effectors, and the renderer's grid
// of them.

// 6 x 6 x 10 cells, each listing up to 127 sphere indices, then -1.
const uint GridCells = 360u;
const uint GridEntries = 128u;

// Two vec4s a sphere: {center xyz, radii (outer and inner half floats)}, {..., mask and strength
// half float}.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Effectors {
    mat4 view;
    float depth;
    float start;
    float slice;
    uint count;
    float projection_x;
    float projection_y;
    uint unused[2];
    uvec4 spheres[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) buffer Grid {
    uint entries[];
};

// The first entry of the grid cell that holds a point of the game's clip space.
uint GridList(vec4 clip, float cluster_scale) {
    const uvec3 cell = min(uvec3(6.0 * clamp(0.5 * clip.x / clip.w + 0.5, 0.0, 1.0),
                                 6.0 * clamp(-0.5 * clip.y / clip.w + 0.5, 0.0, 1.0),
                                 10.0 * clamp(clip.z * cluster_scale, 0.0, 1.0)),
                           uvec3(5u, 5u, 9u));
    return ((cell.z * 6u + cell.y) * 6u + cell.x) * GridEntries;
}
