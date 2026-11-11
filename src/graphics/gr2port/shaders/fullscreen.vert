// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

layout(location = 0) out vec2 position;

// A triangle over the whole target, behind everything in reverse depth.
void main() {
    position = vec2(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex & 2) * 2 - 1));
    gl_Position = vec4(position, 0.0, 1.0);
}
