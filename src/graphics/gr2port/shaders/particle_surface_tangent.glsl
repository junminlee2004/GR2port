// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

// What the game's mesh particle surface programs with a tangent share beside
// particle_surface.glsl: the normal map's place moved by parallax through the heights in the
// normal map's alpha, the mesh's place back from it, and the normal map turned by the tangent
// frame.

#include "particle_surface.glsl"

// u_fParallax: how far the view moves the normal map's place at height 0.
const uint Parallax = 33u;

// The game's conversion to an integer: NaN gives 0, other values are clamped to the int range.
int GameInt(float value) {
    return isnan(value) ? 0 : int(clamp(value, -2147483648.0, 2147483520.0));
}

// The normal map's place: its repeat times the mesh's texture coordinates, plus its offset.
vec2 NormalPlace(ParticleMaterial material, vec2 texcoord) {
    return vec2(material.pixel_constants[NormalTransform + 2u] * texcoord.x +
                    material.pixel_constants[NormalTransform],
                material.pixel_constants[NormalTransform + 3u] * texcoord.y +
                    material.pixel_constants[NormalTransform + 1u]);
}

// The mesh's texture coordinates at a place of the normal map: the place turned back through the
// normal map's offset and repeat.
vec2 MeshPlaceAt(ParticleMaterial material, vec2 normal_place) {
    const vec2 offset = vec2(material.pixel_constants[NormalTransform],
                             material.pixel_constants[NormalTransform + 1u]);
    const vec2 repeat = vec2(material.pixel_constants[NormalTransform + 2u],
                             material.pixel_constants[NormalTransform + 3u]);
    const vec2 inverse = vec2(1.0 / repeat.x, 1.0 / repeat.y);
    return vec2(-offset.x * inverse.x + inverse.x * normal_place.x,
                -offset.y * inverse.y + inverse.y * normal_place.y);
}

// The normal map's place where the view meets the heights in its alpha, 1 where its handle is 0.
// From height 1 down, in 14 - 6 z steps for the unit normal's z, the place moves against `view`
// until a height rises above the step's. The meeting height is interpolated between the last two
// steps, and the place moves by `view` times 1 less that height.
vec2 ParallaxPlace(ParticleMaterial material, vec2 place, vec2 view, float normal_z) {
    // The gradients the game's program takes: the place plus its difference across the quad, less
    // the place.
    const vec2 dx = (place + dFdxCoarse(place)) - place;
    const vec2 dy = (place + dFdyCoarse(place)) - place;
    // A broken normal's z still takes at most the 20 steps of -1.
    const int steps = clamp(GameInt(-6.0 * normal_z + 14.0), 0, 20);
    const float step = 1.0 / float(steps);
    const vec2 step_offset = step * view;
    const uint handle = material.maps[NormalMap];
    vec2 at = place;
    float depth = 1.0;
    float height = 0.0;
    float above = 1.0;
    for (int i = 0; i < steps; i++) {
        at -= step_offset;
        height = handle == 0u
                     ? 1.0
                     : textureGrad(sampler2D(textures[nonuniformEXT(handle & TextureSlotMask)],
                                             texture_sampler),
                                   at, dx, dy)
                           .a;
        depth -= step;
        if (height > depth) {
            break;
        }
        above = height;
    }
    const float above_depth = step + depth;
    const float before = above_depth - above;
    const float after = depth - height;
    float meet = 0.0;
    if (before != after) {
        const float lower = depth * before;
        meet = (above_depth * -after + lower) * (1.0 / (before - after));
    }
    return view * -(1.0 - meet) + place;
}

// HalfOverlay with its difference in one fused step, where the game's programs take it so: green's
// and blue's in particle_surface_tangent.frag, which takes red's from the rounded base as
// HalfOverlay gives it, and all three in particle_surface_tangent_color.frag.
float FusedHalfOverlay(float factor, float other, float layer) {
    const float base = factor * other;
    const bool bright = 0.5 <= base;
    const float flip = bright ? -1.0 : 0.0;
    const float product = layer * base;
    float overlay = 2.0 * product + (bright ? 1.0 : 0.0);
    overlay = 2.0 * product * flip + overlay;
    overlay = (-2.0 * ((1.0 - layer) * base) + fma(-layer, 2.0, 2.0)) * flip + overlay;
    return 0.5 * fma(factor, -other, overlay) + base;
}

// The normal map's normal turned by the tangent frame: the unit tangent, the bitangent (the unit
// normal across the unit tangent, times the sign of the tangent's w) and the unit normal.
vec3 FrameNormal(vec3 map_normal, vec4 tangent, vec3 normal) {
    const vec3 unit_tangent = Normalized(tangent.xyz);
    const float handedness = float(int(0.0 < tangent.w) - int(0.0 > tangent.w));
    const vec3 bitangent = vec3(unit_tangent.y * -normal.z + normal.y * unit_tangent.z,
                                unit_tangent.z * -normal.x + normal.z * unit_tangent.x,
                                unit_tangent.x * -normal.y + normal.x * unit_tangent.y) *
                           handedness;
    return map_normal.z * normal + (map_normal.y * bitangent + map_normal.x * unit_tangent);
}
