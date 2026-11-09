// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "common/logging/log.h"
#include "input/game_functions.h"
#include "video_core/gr2port/hooks.h"
#include "video_core/gr2port/player_control.h"
#include "video_core/gr2port/settings.h"

namespace Gr2Port::PlayerControl {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// void FUN_008f6430(controls, ...): the player's controls, once a frame.
constexpr Hooks::Entry Controls{0x8f6430, 6, Hooks::FramePrologue};

// The Controls option's control type, an int, 0 to 4 for Type A to Type E.
constexpr VAddr ControlType = 0x1f2cab8;

// The player's gravity style in the controls, a u32: 0 Normal, 1 Lunar, 2 Jupiter.
constexpr u64 StyleOffset = 0x5548;

VAddr g_base = 0;
std::atomic<u32> g_kept_type{0};

void PS4_SYSV_ABI OnControls(u32, const Hooks::Registers* registers, u64) {
    const u32 type = *reinterpret_cast<const u32*>(g_base + ControlType - GoldHenBase);
    const u32 style = *reinterpret_cast<const u32*>(registers->rdi + StyleOffset);
    if (type >= Input::GameFunctions::TypeCount || style > 2) {
        return;
    }
    Input::GameFunctions::SetGameState(type, static_cast<Input::GameFunctions::Style>(style));
    if (g_kept_type.exchange(type) != type) {
        Settings::Values values = Settings::Get();
        values.mouse.control_type = type;
        Settings::Set(values);
    }
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    const u32 type = Settings::Get().mouse.control_type;
    g_kept_type.store(type);
    Input::GameFunctions::SetControlType(type);
    const Hooks::Result result =
        Hooks::ObserveEntry(module_base, Controls, c, capacity, &OnControls, 0);
    if (result != Hooks::Result::Hooked && result != Hooks::Result::Chained) {
        LOG_ERROR(Render_Vulkan, "GR2port: the player's controls are not the expected code");
        return false;
    }
    return true;
}

} // namespace Gr2Port::PlayerControl
