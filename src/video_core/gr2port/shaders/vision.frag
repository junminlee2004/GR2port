// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"
#include "surface.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Characters scene_characters;
};

// character.vert's outputs.
layout(location = 3) in vec2 texcoord;
layout(location = 7) flat in uint draw_index;

layout(location = 0) out vec4 color;

// A texel of the screen at uv, as a point sampler reads it.
float ScreenTexel(uint handle, vec2 uv) {
    const ivec2 size = textureSize(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), 0);
    const ivec2 texel = clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - 1);
    return texelFetch(sampler2D(textures[nonuniformEXT(handle)], texture_sampler), texel, 0).r;
}

// A movie frame in the game's surface: luma in the top two thirds, then rows of chroma pairs, U
// in the even column and V in the odd one, in video range, raised to the power 2.2.
vec3 MovieColor(uint handle, vec2 uv, vec4 movie_size) {
    const float column = floor(movie_size.y * uv.x);
    const bool even = mod(column, 2.0) == 0.0;
    const float u_x = even ? uv.x : uv.x - movie_size.x;
    const float v_x = even ? uv.x + movie_size.x : uv.x;
    const float chroma_y = 1.0 - uv.y / 3.0;
    const float u = ScreenTexel(handle, vec2(u_x, chroma_y));
    const float v = ScreenTexel(handle, vec2(v_x, chroma_y));
    const float y = 1.1644 * ScreenTexel(handle, vec2(uv.x, 2.0 / 3.0 * (1.0 - uv.y))) - 0.0729;
    const vec3 rgb = vec3(y + 1.596 * v - 0.798, y - 0.3917 * u + 0.1958 - 0.8129 * v + 0.4065,
                          y + 2.0172 * u - 1.0086);
    return exp2(2.2 * log2(abs(rgb)));
}

// The game's G2_BG_Vision pixel program: the screen's movie, or its texture when it has none,
// under the damage map by its alpha, times the scrolling overlay, plus an offset; the glow's draw
// scales it by the blend constant the game's blend applies.
void main() {
    const uint index = scene_draws.draws[draw_index].material - 1u;
    VisionConstants m =
        VisionConstants(uint64_t(scene_characters) + uint64_t(index) * CharacterBytes);
    if (ScreenDoorShut(m.punch_through)) {
        discard;
    }
    // A screen whose texture is not ready yet, as a new movie's first frame, shows black.
    const uint screen = scene_draws.draws[draw_index].texture;
    const vec3 picture = screen == 0u              ? vec3(0.0)
                         : m.movie_size.y > 1.0 ? MovieColor(screen, texcoord, m.movie_size)
                                                : Sample(screen, texcoord).rgb;
    const uint damage_map = scene_characters.characters[index].normal_map;
    const vec4 damage = damage_map != 0u ? Sample(damage_map, texcoord) : vec4(0.0);
    const vec3 overlay =
        Sample(scene_characters.characters[index].reflection,
               m.post_texcoord.xy + m.post_texcoord.zw * texcoord)
            .rgb;
    const vec3 screen_color = mix(picture, damage.rgb, damage.a);
    color = vec4(m.glow * clamp(overlay * screen_color + m.movie_size.z, 0.0, 1.0), 1.0);
}
