// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(push_constant) uniform Constants {
    layout(offset = 24) uint texture_handle;
};

layout(location = 0) in vec2 texcoord;
layout(location = 1) in vec4 color;

layout(location = 0) out vec4 frag_color;

// As the emulator's ImGui backend: the vertex colour times the texture, bilinear and repeating.
void main() {
    frag_color = color * texture(sampler2D(textures[texture_handle], texture_sampler), texcoord);
}
