// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// What the fragment shaders of the world's surfaces share.

// The game's packed normal: the direction of xy, with the length sqrt(0.5 - 0.5 z).
vec2 PackNormal(vec3 n) {
    return n.xy * inversesqrt(max(dot(n.xy, n.xy), 1e-6)) * sqrt(abs(0.5 - 0.5 * n.z));
}

// A screen door fades a surface in. The game's depth runs the other way from this one.
bool ScreenDoorShut(float threshold) {
    const float seed = 256.0 * (0.0771356 * gl_FragCoord.x + 0.00888381 * gl_FragCoord.y) *
                           (1.0 - gl_FragCoord.z) +
                       1e-7;
    return threshold < fract(fract(abs(123456792.0 / seed)) * abs(seed));
}
