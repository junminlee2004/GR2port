// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

layout(location = 0) in vec3 view_position;
layout(location = 1) flat in uint mesh_id;

layout(location = 0) out vec4 color;

vec3 Tint(uint h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return vec3(uvec3(h, h >> 8, h >> 16) & 0xffu) / 255.0;
}

// Untextured: a tint per mesh, lit from the eye by the face normal.
void main() {
    const vec3 normal = normalize(cross(dFdx(view_position), dFdy(view_position)));
    const float facing = abs(dot(normal, normalize(view_position)));
    color = vec4((0.45 + 0.4 * Tint(mesh_id)) * (0.25 + 0.75 * facing), 1.0);
}
