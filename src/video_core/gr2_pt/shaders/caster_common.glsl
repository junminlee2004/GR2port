// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The shadow casters' depth transform. The math and its order of operations follow the game's
// vertex programs: rows are the game's row-major matrices, and positions multiply from the left.

#extension GL_ARB_shader_draw_parameters : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Rows {
    vec4 rows[];
};

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words {
    uint words[];
};

// A caster's draw: the device addresses of vertex 0 of its streams with their strides, and of
// its matrices.
struct Draw {
    Words positions;
    Words weights;
    Words bone_indices;
    Rows world;
    Rows view_projection;
    Rows palette;
    uint position_stride;
    uint weight_stride;
    uint bone_stride;
    uint bones;
};

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Draws {
    Draw draws[];
};

// Draws points at the first draw of the indirect draw call.
layout(push_constant, std430) uniform Constants {
    Draws draws;
    uint flags;
} pc;

// The clip space is [-w, w] and the device has no depth clip control.
const uint FlagDepthMinusOneToOne = 1u;

out gl_PerVertex {
    vec4 gl_Position;
};

Words Vertex(Words stream, uint stride) {
    return Words(uint64_t(stream) + uint64_t(uint(gl_VertexIndex)) * stride);
}

vec3 Position(Draw d) {
    Words v = Vertex(d.positions, d.position_stride);
    return vec3(uintBitsToFloat(v.words[0]), uintBitsToFloat(v.words[1]),
                uintBitsToFloat(v.words[2]));
}

// The world position, then the clip position, whose z is clamped to -w in front of the light.
vec4 Clip(Draw d, vec3 p) {
    Rows w = d.world;
    const vec4 world =
        ((w.rows[3] + w.rows[2] * p.z) + w.rows[1] * p.y) + w.rows[0] * p.x;
    Rows v = d.view_projection;
    vec4 clip = ((v.rows[3] * world.w + v.rows[2] * world.z) + v.rows[1] * world.y) +
                v.rows[0] * world.x;
    if (0.5 < clip.w) {
        clip.z = max(-clip.w, clip.z);
    }
    if ((pc.flags & FlagDepthMinusOneToOne) != 0u) {
        clip.z = (clip.z + clip.w) * 0.5;
    }
    return clip;
}
