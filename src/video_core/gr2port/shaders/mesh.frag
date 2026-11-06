// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;
layout(set = 1, binding = 0) uniform sampler2DArrayShadow shadow_map;

layout(push_constant) uniform Constants {
    Draws scene_draws;
    Shadows shadows;
};

layout(location = 0) in vec3 view_position;
layout(location = 1) in vec2 texcoord;
layout(location = 2) flat in vec3 color_scale;
layout(location = 3) flat in uint texture_index;

layout(location = 0) out vec4 color;

const float Ambient = 0.4;
// Cascades are used inside this part of their square, which leaves room for the filter.
const float CascadeInner = 0.98;

vec3 LinearToSrgb(vec3 c) {
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// 1 where the key light reaches the point, from the first cascade that covers it. Cascades are
// drawn with a flipped viewport, as the main view, so their rows run from y = 1 down.
float Shadow(vec3 position) {
    const vec4 world = shadows.inverse_view * vec4(position, 1.0);
    for (uint c = 0u; c < Cascades; c++) {
        const vec4 clip = shadows.view_projection[c * CasterTables] * world;
        const vec3 ndc = clip.xyz / clip.w;
        if (all(lessThan(abs(ndc.xy), vec2(CascadeInner))) && abs(ndc.z) <= 1.0) {
            const vec2 uv = vec2(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y);
            return texture(shadow_map, vec4(uv, float(c), 0.5 + 0.5 * ndc.z));
        }
    }
    return 1.0;
}

// The albedo, lit by the key light with the face normal and the shadow cascades. The swapchain is
// UNORM, so the shader encodes sRGB itself.
void main() {
    const vec3 albedo =
        texture(sampler2D(textures[nonuniformEXT(texture_index)], texture_sampler), texcoord).rgb *
        color_scale;
    vec3 normal = normalize(cross(dFdx(view_position), dFdy(view_position)));
    normal = dot(normal, view_position) > 0.0 ? -normal : normal;
    const float diffuse = max(dot(normal, shadows.light_direction.xyz), 0.0);
    const float light = Ambient + (1.0 - Ambient) * diffuse * Shadow(view_position);
    color = vec4(LinearToSrgb(albedo * light), 1.0);
}
