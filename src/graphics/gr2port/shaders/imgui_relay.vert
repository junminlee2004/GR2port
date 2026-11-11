// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_buffer_reference : require

// ImGui's vertices: a position, texture coordinates and a colour of four bytes, five words each.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Vertices {
    uint words[];
};

layout(push_constant) uniform Constants {
    Vertices vertices;
    vec2 scale;
    vec2 translate;
    uint texture_handle;
};

layout(location = 0) out vec2 texcoord;
layout(location = 1) out vec4 color;

void main() {
    const uint base = uint(gl_VertexIndex) * 5;
    const vec2 position =
        uintBitsToFloat(uvec2(vertices.words[base], vertices.words[base + 1]));
    texcoord = uintBitsToFloat(uvec2(vertices.words[base + 2], vertices.words[base + 3]));
    color = unpackUnorm4x8(vertices.words[base + 4]);
    gl_Position = vec4(position * scale + translate, 0.0, 1.0);
}
