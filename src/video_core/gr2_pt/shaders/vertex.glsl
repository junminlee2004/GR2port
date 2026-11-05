// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Vertex pulling: a mesh's elements at gl_VertexIndex. An element holds a byte offset in its low 8
// bits and, above them, an index into the game's vertex format table.

// Format indices of the game's vertex format table.
const uint FormatRg32Float = 2u;
const uint FormatRg16Float = 6u;
const uint FormatRgba16Float = 7u;
const uint FormatRg16Sint = 12u;

Words Element(uint64_t vertices, uint stride, uint element) {
    return Words(vertices + uint64_t(gl_VertexIndex) * stride + (element & 0xffu));
}

vec3 LoadPosition(uint64_t vertices, uint stride, uint position) {
    Words words = Element(vertices, stride, position);
    if ((position >> 8) == FormatRgba16Float) {
        return vec3(unpackHalf2x16(words.words[0]), unpackHalf2x16(words.words[1]).x);
    }
    return uintBitsToFloat(uvec3(words.words[0], words.words[1], words.words[2]));
}

// A skinned position: the blend of four bones by their weights, in the order of operations of the
// game's vertex programs. The skin stream holds four RGBA16 FLOAT weights, then four RGBA16 SINT
// bone indices.
vec3 SkinPosition(vec3 p, uint64_t skin, uint64_t palette, uint bones) {
    Words words = Words(skin + uint64_t(gl_VertexIndex) * 16u);
    const vec2 w01 = unpackHalf2x16(words.words[0]);
    const vec2 w23 = unpackHalf2x16(words.words[1]);
    const int b0 = bitfieldExtract(int(words.words[2]), 0, 16);
    const int b1 = bitfieldExtract(int(words.words[2]), 16, 16);
    const int b2 = bitfieldExtract(int(words.words[3]), 0, 16);
    const int b3 = bitfieldExtract(int(words.words[3]), 16, 16);
    Rows rows = Rows(palette);
    vec3 skinned;
    for (uint r = 0u; r < 3u; r++) {
        const vec4 row =
            ((BoneRow(rows, bones, b0, r) * w01.x + BoneRow(rows, bones, b1, r) * w01.y) +
             BoneRow(rows, bones, b2, r) * w23.x) +
            BoneRow(rows, bones, b3, r) * w23.y;
        skinned[r] = ((row.w + row.x * p.x) + row.y * p.y) + row.z * p.z;
    }
    return skinned;
}

vec2 LoadTexcoord(uint64_t vertices, uint stride, uint element) {
    const uint format = element >> 8;
    if (format == 0u) {
        return vec2(0.0);
    }
    Words words = Element(vertices, stride, element);
    if (format == FormatRg16Sint) {
        // The game's vertex shaders scale 16-bit integer texture coordinates by 1/1024.
        const uint word = words.words[0];
        return vec2(int(word << 16) >> 16, int(word) >> 16) / 1024.0;
    }
    if (format == FormatRg16Float) {
        return unpackHalf2x16(words.words[0]);
    }
    return uintBitsToFloat(uvec2(words.words[0], words.words[1]));
}
