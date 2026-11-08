// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What the game's lit programs share about the point lights: the cell of LightCompute's grid a
// pixel reads, and a light's falloff. Needs scene.glsl.

// The first light index of the cell a pixel at uv, at a view depth, falls in: the slices double
// in depth beyond start, the near plane's distance.
uint GridCellFirst(vec2 uv, float depth, float start) {
    const float beyond = depth - start;
    const uint slice = beyond > 1.0 ? min(uint(log2(beyond)), LightGridSlices - 1u) : 0u;
    const uvec2 tile = uvec2(vec2(LightGridColumns, LightGridRows) * uv);
    return ((slice * LightGridRows + tile.y) * LightGridColumns + tile.x) * LightsPerCell;
}

// The share of a light left at a distance: it falls by the light's power from where its falloff
// starts to none at its range.
float PointFade(PointLight point, float reach) {
    const float falloff =
        min(max(reach - point.falloff_start, 0.0) / (point.range - point.falloff_start), 1.0);
    return 1.0 - (falloff > 0.0 ? exp2(point.falloff_power * log2(falloff)) : 0.0);
}
