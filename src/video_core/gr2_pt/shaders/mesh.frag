// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(location = 0) in vec3 view_position;
layout(location = 1) in vec2 texcoord;
layout(location = 2) flat in vec3 color_scale;
layout(location = 3) flat in uint texture_index;

layout(location = 0) out vec4 color;

vec3 LinearToSrgb(vec3 c) {
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// The albedo, lit from the eye by the face normal. The swapchain is UNORM, so the shader encodes
// sRGB itself.
void main() {
    const vec3 albedo =
        texture(sampler2D(textures[nonuniformEXT(texture_index)], texture_sampler), texcoord).rgb *
        color_scale;
    const vec3 normal = normalize(cross(dFdx(view_position), dFdy(view_position)));
    const float facing = abs(dot(normal, normalize(view_position)));
    color = vec4(LinearToSrgb(albedo * (0.35 + 0.65 * facing)), 1.0);
}
