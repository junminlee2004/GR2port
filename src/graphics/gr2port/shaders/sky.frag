// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

// Cube maps and 2D textures share the texture array; a handle's kind picks the declaration.
layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 0) uniform textureCube cubes[];
layout(set = 0, binding = 1) uniform sampler repeat_sampler;
layout(set = 0, binding = 2) uniform sampler clamp_sampler;

layout(push_constant) uniform Constants {
    Sky sky;
};

layout(location = 0) in vec2 position;

layout(location = 0) out vec4 color;

vec3 CubeMap(uint index, vec3 ray) {
    return textureLod(samplerCube(cubes[sky.cubes[index]], clamp_sampler),
                      mat3(sky.transforms[index]) * ray, 0.0)
        .rgb;
}

// A layer's cloud density at a point of the plane the clouds lie in, fading with distance.
float CloudLayer(vec4 layer, vec2 plane) {
    const vec2 scaled = layer.x * plane;
    const float density = textureLod(sampler2D(textures[sky.cloud], repeat_sampler),
                                     (layer.yz + scaled) * layer.w, 0.0)
                              .r;
    const float fade = sky.cloud_fade.y * (length(scaled) - sky.cloud_fade.x);
    return density * (1.0 - clamp(fade, 0.0, 1.0));
}

// The game's sky programs: the cube maps, the sun's disc, then two layers of cloud above the
// horizon.
void main() {
    const vec3 ray = normalize((sky.inverse_view_projection * vec4(position, 1.0, 1.0)).xyz);
    vec3 result = CubeMap(0u, ray);
    if (sky.blend > 0.0) {
        result = mix(result, CubeMap(1u, ray), sky.blend);
    }
    const float angle = acos(max(dot(sky.sun_direction.xyz, ray), 0.0));
    const float sun = 1.0 - min(max(angle - sky.sun_size, 0.0) * sky.sun_outer, 1.0);
    const float sun_squared = sun * sun;
    result = mix(result, sky.sun_color.rgb, sun_squared * sun_squared);
    if (ray.y > 0.0 && sky.cloud_color.a > 0.0) {
        const vec2 plane = ray.xz / ray.y;
        const float density =
            max(CloudLayer(sky.cloud_layers[0], plane), CloudLayer(sky.cloud_layers[1], plane));
        result = mix(result, sky.cloud_color.rgb, min(density, 1.0) * sky.cloud_color.a);
    }
    color = vec4(result, 1.0);
}
