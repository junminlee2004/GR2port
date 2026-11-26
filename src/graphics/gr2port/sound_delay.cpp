// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/sound_delay.h"

namespace Gr2Port::SoundDelay {

namespace {

// s32 sceSndxInstanceSetPreDelay(float seconds, s32 instance), FUN_006765f0: only the game's two
// paths that start sound effects and voices call it, with 1/6 s, just before the start.
constexpr Hooks::Entry SetPreDelay{0x6765f0, 6, Hooks::FramePrologue};

// The frame rate patches' length of this game frame in 30 fps frames, at most 1, a float.
constexpr VAddr FrameShare = 0x1f7c600;

// The frames the picture takes from the game logic to the swapchain.
constexpr float PictureFrames = 2.0f;
constexpr float FrameRate = 30.0f;

using SetPreDelayFunction = s32 PS4_SYSV_ABI (*)(float seconds, s32 instance);

VAddr g_module_base = 0;
SetPreDelayFunction g_set_pre_delay = nullptr;

s32 PS4_SYSV_ABI OnSetPreDelay(float seconds, s32 instance) {
    float share;
    std::memcpy(&share, Hooks::Code(g_module_base, FrameShare), sizeof(share));
    return Core::Recompiled::Call(g_set_pre_delay,
                                  seconds - PictureFrames * (1.0f - share) / FrameRate, instance);
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_module_base = module_base;
    const void* original = nullptr;
    if (Hooks::Replace(module_base, SetPreDelay, c, capacity,
                       reinterpret_cast<const void*>(&OnSetPreDelay),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the sounds' pre-delay is not the expected code");
        return false;
    }
    g_set_pre_delay = reinterpret_cast<SetPreDelayFunction>(const_cast<void*>(original));
    return true;
}

} // namespace Gr2Port::SoundDelay
