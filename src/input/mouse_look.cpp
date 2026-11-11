// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <mutex>

#include "input/mouse_look.h"

namespace Input::MouseLook {

namespace {

using Clock = std::chrono::steady_clock;

// A camera that took no movement for this long has stopped updating.
constexpr auto Stale = std::chrono::milliseconds(200);

std::mutex g_mutex;
float g_dx = 0.0f;
float g_dy = 0.0f;
Clock::time_point g_taken{};

} // namespace

void Add(float dx, float dy) {
    std::scoped_lock lock{g_mutex};
    if (Clock::now() - g_taken > Stale) {
        g_dx = 0.0f;
        g_dy = 0.0f;
        return;
    }
    g_dx += dx;
    g_dy += dy;
}

bool Taken() {
    std::scoped_lock lock{g_mutex};
    return Clock::now() - g_taken <= Stale;
}

std::pair<float, float> Take() {
    std::scoped_lock lock{g_mutex};
    g_taken = Clock::now();
    const std::pair movement{g_dx, g_dy};
    g_dx = 0.0f;
    g_dy = 0.0f;
    return movement;
}

} // namespace Input::MouseLook
