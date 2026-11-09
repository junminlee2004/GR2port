// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <SDL3/SDL_gamepad.h>

#include "core/emulator_settings.h"
#include "input/selected_pad.h"

namespace Input::SelectedPad {

namespace {

std::mutex g_mutex;
std::string g_picked;

} // namespace

std::string Guid() {
    {
        std::scoped_lock lock{g_mutex};
        if (!g_picked.empty()) {
            return g_picked;
        }
    }
    return EmulatorSettings.GetDefaultControllerId();
}

void Set(std::string guid) {
    std::scoped_lock lock{g_mutex};
    g_picked = std::move(guid);
}

std::string GuidOf(SDL_JoystickID id) {
    char text[33]{};
    SDL_GUIDToString(SDL_GetGamepadGUIDForID(id), text, sizeof(text));
    return text;
}

} // namespace Input::SelectedPad
