// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "scene.glsl"

layout(push_constant) uniform Constants {
    Draws scene_draws;
};

layout(location = 0) flat in uint draw_index;

layout(location = 0) out vec4 color;

// The game's SelfTranslucent pixel program: one colour.
void main() {
    Draws draws = scene_draws;
    color = draws.draws[draw_index].color_scale;
}
