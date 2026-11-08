// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 1, binding = 0) uniform sampler samplers[4];

layout(push_constant) uniform Constants {
    uint picture;
};

layout(location = 0) in vec2 position;

layout(location = 0) out vec4 color;

// The frame's back buffer over the window, through the clamping bilinear sampler.
void main() {
    const vec2 texcoord = 0.5 * position + 0.5;
    const vec3 picture_color = texture(sampler2D(textures[picture], samplers[1]), texcoord).rgb;
    color = vec4(picture_color, 1.0);
}
