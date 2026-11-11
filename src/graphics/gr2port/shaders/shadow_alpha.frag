// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 0) uniform texture2D textures[];
layout(set = 0, binding = 1) uniform sampler texture_sampler;

layout(location = 0) in vec2 texcoord;
layout(location = 1) flat in uint texture_index;
layout(location = 2) flat in float alpha_reference;

// Texels with less alpha than the caster's reference let the light through.
void main() {
    const float alpha =
        texture(sampler2D(textures[nonuniformEXT(texture_index)], texture_sampler), texcoord).a;
    if (alpha < alpha_reference) {
        discard;
    }
}
