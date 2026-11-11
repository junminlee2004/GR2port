// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
    Decals scene_decals;
    Light light;
    vec2 target_inverse;
};

layout(location = 3) in vec2 texcoord;
layout(location = 7) flat in uint draw_index;

vec4 Sample(uint handle, vec2 uv) {
    return texture(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), uv);
}

// A character's depth for the ambient occlusion: the pixels G2_CH_Main keeps, past its alpha step
// and its screen door, as character.frag decides them.
void main() {
    Characters table = scene_characters;
    const uint index = scene_draws.draws[draw_index].material - 1u;
    const vec2 uv = texcoord;
    if (table.characters[index].alpha_step != 0u) {
        const float mask = Sample(scene_draws.draws[draw_index].texture, uv).a;
        const vec2 height_uv =
            table.characters[index].normal_offset + table.characters[index].normal_repeat * uv;
        const float height = pow(abs(Sample(table.characters[index].normal_map, height_uv).a), 2.2);
        if (table.characters[index].alpha_reference >= height * mask) {
            discard;
        }
    }
    const vec2 punch = table.characters[index].punch_through;
    const float seed =
        256.0 * punch.x * (0.0771356 * gl_FragCoord.x + 0.00888381 * gl_FragCoord.y) + 1e-7;
    if (punch.y < fract(fract(abs(123456792.0 / seed)) * abs(seed))) {
        discard;
    }
}
