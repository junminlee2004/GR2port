// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460
#extension GL_GOOGLE_include_directive : require

#include "caster_common.glsl"

void main() {
    Draw d = CurrentDraw();
    gl_Position = Clip(d, Position(d));
}
