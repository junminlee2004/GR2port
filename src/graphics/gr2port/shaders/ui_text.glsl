// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// Gives text its 16:9 shape on a wider or taller screen, where the game stretches its whole screen
// drawing. A text draw is one string; it narrows by the scale around the point the game lays it
// out from, the origin of its matrix, so it keeps its alignment and its letters keep their
// spacing. Text without that matrix narrows around each triangle's middle. Everything else stays
// as the game draws it. TEXT_ATTRIBUTES is the vertex program's outputs, 1 or 2.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(push_constant) uniform Constants {
    uint64_t draws_address;
    vec2 text_scale;
};

// Scene::UiDraw: the words of its constants' address and of its flags, and the flags.
layout(buffer_reference, std430, buffer_reference_align = 4) buffer Words {
    uint words[];
};
const uint DrawBytes = 160u;
const uint DrawConstants = 2u;
const uint DrawFlags = 38u;
const uint DrawText = 1u;
const uint DrawAnchored = 2u;

layout(location = 15) flat in uint draw_index_in[];
layout(location = 0) in vec4 attr0_in[];
#if TEXT_ATTRIBUTES == 2
layout(location = 1) in vec4 attr1_in[];
#endif

layout(location = 15) flat out uint draw_index;
layout(location = 0) out vec4 attr0;
#if TEXT_ATTRIBUTES == 2
layout(location = 1) out vec4 attr1;
#endif

void main() {
    Words draw = Words(draws_address + uint64_t(draw_index_in[0]) * DrawBytes);
    const uint flags = draw.words[DrawFlags];
    bool narrow = (flags & DrawText) != 0u;
    vec2 low = vec2(1.0e30);
    vec2 high = vec2(-1.0e30);
    for (int i = 0; i < 3; i++) {
        const vec4 position = gl_in[i].gl_Position;
        narrow = narrow && position.w > 0.0;
        low = min(low, position.xy / position.w);
        high = max(high, position.xy / position.w);
    }
    vec2 anchor = 0.5 * (low + high);
    if (narrow && (flags & DrawAnchored) != 0u) {
        // The matrix's translation: words 12 and 13 give x and y, word 15 gives w.
        Words constants = Words(uint64_t(draw.words[DrawConstants]) |
                                uint64_t(draw.words[DrawConstants + 1u]) << 32);
        const float w = uintBitsToFloat(constants.words[15]);
        if (abs(w) > 1.0e-6) {
            anchor = vec2(uintBitsToFloat(constants.words[12]),
                          uintBitsToFloat(constants.words[13])) / w;
        }
    }
    for (int i = 0; i < 3; i++) {
        vec4 position = gl_in[i].gl_Position;
        if (narrow) {
            position.xy = (anchor + (position.xy / position.w - anchor) * text_scale) * position.w;
        }
        gl_Position = position;
        draw_index = draw_index_in[i];
        attr0 = attr0_in[i];
#if TEXT_ATTRIBUTES == 2
        attr1 = attr1_in[i];
#endif
        EmitVertex();
    }
}
