// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <set>
#include <tuple>

#include "launcher/qt/displays.h"

namespace Gr2Port::Launcher {

std::vector<Monitor> ListMonitors() {
    std::vector<Monitor> monitors;
    int count = 0;
    SDL_DisplayID* const ids = SDL_GetDisplays(&count);
    if (ids == nullptr) {
        return monitors;
    }
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; ++i) {
        SDL_Rect area{};
        if (!SDL_GetDisplayBounds(ids[i], &area)) {
            continue;
        }
        const char* const name = SDL_GetDisplayName(ids[i]);
        monitors.push_back({ids[i],
                            std::to_string(i + 1) + " (" + (name != nullptr ? name : "") + ")",
                            area, ids[i] == primary});
    }
    SDL_free(ids);
    return monitors;
}

Modes ListModes(SDL_DisplayID monitor) {
    std::set<std::tuple<u32, u32, u32>> found;
    const auto add = [&found](const SDL_DisplayMode& mode) {
        const long rate = std::lround(mode.refresh_rate);
        if (mode.w >= 800 && mode.h >= 600 && rate > 1) {
            found.emplace(static_cast<u32>(mode.w), static_cast<u32>(mode.h),
                          static_cast<u32>(rate));
        }
    };
    int count = 0;
    if (SDL_DisplayMode** const modes = SDL_GetFullscreenDisplayModes(monitor, &count)) {
        for (int i = 0; i < count; ++i) {
            add(*modes[i]);
        }
        SDL_free(modes);
    }
    Modes modes;
    if (const SDL_DisplayMode* const current = SDL_GetCurrentDisplayMode(monitor)) {
        modes.current_width = static_cast<u32>(current->w);
        modes.current_height = static_cast<u32>(current->h);
        modes.current_rate = static_cast<u32>(std::lround(current->refresh_rate));
        add(*current);
    }
    for (const auto& [width, height, rate] : found) {
        if (modes.sizes.empty() || modes.sizes.back().width != width ||
            modes.sizes.back().height != height) {
            modes.sizes.push_back({width, height, {}});
        }
        modes.sizes.back().rates.push_back(rate);
    }
    return modes;
}

} // namespace Gr2Port::Launcher
