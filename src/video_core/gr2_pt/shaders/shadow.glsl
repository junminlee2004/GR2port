// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// A shadow caster's vertex, for shadow.vert and, with ALPHA_TEST, shadow_alpha.vert.

#include "scene.glsl"
#include "vertex.glsl"

layout(push_constant) uniform Constants {
    Casters scene_casters;
    Shadows shadows;
    uint cascade;
};

#ifdef ALPHA_TEST
layout(location = 0) out vec2 texcoord;
layout(location = 1) flat out uint texture_index;
layout(location = 2) flat out float alpha_reference;
#endif

// A static caster in one cascade, as the game's vertex program draws it: the world matrix, then the
// cascade's view-projection, with depth clamped to the near plane (pancaking).
void main() {
    Casters casters = scene_casters;
    const uint i = gl_InstanceIndex;
    const vec4 position = vec4(LoadPosition(casters.casters[i].vertices, casters.casters[i].stride,
                                            casters.casters[i].position),
                               1.0);
    const vec4 world = casters.casters[i].world * position;
    vec4 clip = shadows.view_projection[cascade * CasterTables + casters.casters[i].table] * world;
    if (clip.w > 0.5) {
        clip.z = max(-clip.w, clip.z);
    }
    // OpenGL depth, -w to w, to Vulkan depth, 0 to w.
    clip.z = 0.5 * (clip.z + clip.w);
    gl_Position = clip;
#ifdef ALPHA_TEST
    texcoord = LoadTexcoord(casters.casters[i].vertices, casters.casters[i].stride,
                            casters.casters[i].texcoord);
    texture_index = casters.casters[i].texture;
    alpha_reference = casters.casters[i].alpha_reference;
#endif
}
