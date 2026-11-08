// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(location = 0) in vec2 position;

// The game's bloom pass 0 (PS 0x1bb8334): the scene's depth at the bloom's size, which only the
// glow draws test against, 1e-4 farther, so that a glow passes on its own surface.
void main() {
    gl_FragDepth = max(texture(scene_depth, 0.5 * position + 0.5).r - 1e-4, 0.0);
}
