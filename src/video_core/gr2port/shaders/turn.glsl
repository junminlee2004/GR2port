// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The turns of the game's vegetation kernels, as unit quaternions.

// The turn by twice half_angle about an axis of any length; no turn about no axis.
vec4 Turn(vec3 axis, float half_angle) {
    const float length_squared = dot(axis, axis);
    return length_squared > 0.0
               ? vec4(axis * inversesqrt(length_squared) * sin(half_angle), cos(half_angle))
               : vec4(0.0, 0.0, 0.0, 1.0);
}

vec3 Turned(vec3 v, vec4 turn) {
    return v + 2.0 * cross(turn.xyz, cross(turn.xyz, v) + turn.w * v);
}
