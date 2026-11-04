// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "caster_common.glsl"

// Row r of a bone: each bone is three rows. A bone past the palette reads zeros, as the game's
// buffer loads past the palette's records do.
vec4 BoneRow(Draw d, int bone, uint r) {
    return uint(bone) < d.bones ? d.palette.rows[uint(bone) * 3u + r] : vec4(0.0);
}

void main() {
    Draw d = pc.draws.draws[gl_DrawIDARB];
    const vec3 p = Position(d);
    Words weight_words = Vertex(d.weights, d.weight_stride);
    const vec2 w01 = unpackHalf2x16(weight_words.words[0]);
    const vec2 w23 = unpackHalf2x16(weight_words.words[1]);
    Words bone_words = Vertex(d.bone_indices, d.bone_stride);
    const int b0 = bitfieldExtract(int(bone_words.words[0]), 0, 16);
    const int b1 = bitfieldExtract(int(bone_words.words[0]), 16, 16);
    const int b2 = bitfieldExtract(int(bone_words.words[1]), 0, 16);
    const int b3 = bitfieldExtract(int(bone_words.words[1]), 16, 16);
    vec3 skinned;
    for (uint r = 0u; r < 3u; r++) {
        const vec4 row = ((BoneRow(d, b0, r) * w01.x + BoneRow(d, b1, r) * w01.y) +
                          BoneRow(d, b2, r) * w23.x) +
                         BoneRow(d, b3, r) * w23.y;
        skinned[r] = ((row.w + row.x * p.x) + row.y * p.y) + row.z * p.z;
    }
    gl_Position = Clip(d, skinned);
}
