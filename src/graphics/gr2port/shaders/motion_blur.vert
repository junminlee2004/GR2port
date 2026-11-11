// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

layout(location = 0) out vec4 attr0;

// A triangle over the whole target, passing the screen's texture coordinates, with the top left
// at 0, as the game's full-screen vertex program does for MotionBlur's reconstruction.
void main() {
    const vec2 position =
        vec2(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex & 2) * 2 - 1));
    attr0 = vec4(position * 0.5 + 0.5, 0.0, 1.0);
    gl_Position = vec4(position, 0.0, 1.0);
}
