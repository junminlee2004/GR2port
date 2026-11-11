// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include "common/logging/log.h"
#include "input/controller.h"
#include "input/input_mouse.h"

#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/window.h"

namespace Gr2Port::Window {

namespace {

SDL_Window* g_window = nullptr;
std::atomic<SDL_WindowID> g_window_id{0};
std::atomic<SDL_WindowID> g_game_id{0};
// The pixel size, width in the high 32 bits.
std::atomic<u64> g_size{0};

void StoreSize(int width, int height) {
    g_size.store(static_cast<u64>(width) << 32 | static_cast<u32>(height),
                 std::memory_order_relaxed);
}

bool SDLCALL WatchEvents(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        event->window.windowID == g_window_id.load(std::memory_order_relaxed)) {
        StoreSize(event->window.data1, event->window.data2);
    }
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F2 && !event->key.repeat) {
        Capture::ToggleCapture();
    }
    return true;
}

SDL_DisplayID ChosenDisplay(const Settings::Display& display) {
    if (display.monitor) {
        const SDL_Point point{display.monitor->x, display.monitor->y};
        if (const SDL_DisplayID id = SDL_GetDisplayForPoint(&point); id != 0) {
            return id;
        }
    }
    return SDL_GetPrimaryDisplay();
}

// Exclusive fullscreen switches the monitor to the closest mode; otherwise the window covers the
// monitor at its desktop mode.
void EnterFullscreen(SDL_DisplayID screen, const Settings::Display& display) {
    SDL_DisplayMode mode{};
    const SDL_DisplayMode* wanted = nullptr;
    if (display.exclusive &&
        SDL_GetClosestFullscreenDisplayMode(
            screen, static_cast<int>(display.width), static_cast<int>(display.height),
            static_cast<float>(display.refresh_rate), false, &mode)) {
        wanted = &mode;
    }
    if (!SDL_SetWindowFullscreenMode(g_window, wanted) ||
        !SDL_SetWindowFullscreen(g_window, true)) {
        LOG_ERROR(Render_Vulkan, "GR2port: no fullscreen: {}", SDL_GetError());
    }
}

void SDLCALL CreateOnMainThread(void*) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* const game = count > 0 ? windows[0] : nullptr;
    SDL_free(windows);

    const Settings::Display display = Settings::Get().display;
    const SDL_DisplayID screen = ChosenDisplay(display);
    const int initial_width = static_cast<int>(display.width);
    const int initial_height = static_cast<int>(display.height);
    const SDL_PropertiesID props = SDL_CreateProperties();
    // "Gravity Rush 2(TM)" in UTF-8.
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Gravity Rush 2\xE2\x84\xA2");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(screen));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          SDL_WINDOWPOS_CENTERED_DISPLAY(screen));
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, initial_width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, initial_height);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    // Wayland shows a child window only after its parent has drawn a frame, and nothing draws in
    // the game's window. That window never shows there, so there is no window to close but this
    // one.
    if (game != nullptr && SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") != 0) {
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, game);
    }
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (g_window == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port: no window: {}", SDL_GetError());
        return;
    }
    if (display.fullscreen) {
        EnterFullscreen(screen, display);
    }
    // The mouse turns the camera: while the window has focus, the cursor is hidden and held. Touch
    // pad swipes need a free cursor instead.
    if (!Input::IsTouchpadSwipeEnabled() && !SDL_SetWindowRelativeMouseMode(g_window, true)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the window cannot hold the mouse: {}", SDL_GetError());
    }
    int width = initial_width;
    int height = initial_height;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    StoreSize(width, height);
    if (game != nullptr) {
        g_game_id.store(SDL_GetWindowID(game), std::memory_order_relaxed);
    }
    g_window_id.store(SDL_GetWindowID(g_window), std::memory_order_relaxed);
    SDL_AddEventWatch(&WatchEvents, nullptr);
}

} // namespace

SDL_Window* Create() {
    if (!SDL_RunOnMainThread(&CreateOnMainThread, nullptr, true)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the main thread did not create the window: {}",
                  SDL_GetError());
        return nullptr;
    }
    return g_window;
}

Size PixelSize() {
    const u64 size = g_size.load(std::memory_order_relaxed);
    return {static_cast<u32>(size >> 32), static_cast<u32>(size)};
}

u32 Id() {
    return g_window_id.load(std::memory_order_relaxed);
}

u32 GameId() {
    return g_game_id.load(std::memory_order_relaxed);
}

bool ToggleFullscreen() {
    if (g_window == nullptr) {
        return false;
    }
    if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0) {
        if (!SDL_SetWindowFullscreen(g_window, false)) {
            LOG_ERROR(Render_Vulkan, "GR2port: no windowed mode: {}", SDL_GetError());
        }
    } else {
        EnterFullscreen(SDL_GetDisplayForWindow(g_window), Settings::Get().display);
    }
    return true;
}

} // namespace Gr2Port::Window
