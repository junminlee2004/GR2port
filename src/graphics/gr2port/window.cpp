// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "input/controller.h"
#include "input/input_mouse.h"

#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/vblank.h"
#include "graphics/gr2port/window.h"

namespace Gr2Port::Window {

namespace {

SDL_Window* g_window = nullptr;
std::atomic<SDL_WindowID> g_window_id{0};
// The pixel size, width in the high 32 bits.
std::atomic<u64> g_size{0};
std::mutex g_sizes_mutex;
// Under g_sizes_mutex.
std::vector<Size> g_monitor_sizes;
// Main thread: the settings' display as the window last took it.
Settings::Display g_shown;

void StoreSize(int width, int height) {
    g_size.store(static_cast<u64>(width) << 32 | static_cast<u32>(height),
                 std::memory_order_relaxed);
}

// Whether the print screen key is down. Windows tells SDL of that key only when it is released,
// so there the key's own state is read.
bool PrintScreenHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SNAPSHOT) & 0x8000) != 0;
#else
    return SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_PRINTSCREEN];
#endif
}

// Tells the vblank the refresh rate of the monitor the window is on.
void StoreRefreshRate() {
    const SDL_DisplayMode* const mode =
        SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window));
    double rate = 0.0;
    if (mode != nullptr) {
        rate =
            mode->refresh_rate_denominator > 0
                ? static_cast<double>(mode->refresh_rate_numerator) / mode->refresh_rate_denominator
                : mode->refresh_rate;
    }
    Vblank::SetRefreshRate(rate);
}

// Keeps the sizes of the monitor's modes of at least 800 x 600 and of its current mode, as the
// launcher lists them.
void StoreMonitorSizes() {
    const SDL_DisplayID monitor = SDL_GetDisplayForWindow(g_window);
    std::set<std::pair<u32, u32>> found;
    const auto add = [&found](const SDL_DisplayMode& mode) {
        if (mode.w >= 800 && mode.h >= 600 && std::lround(mode.refresh_rate) > 1) {
            found.emplace(static_cast<u32>(mode.w), static_cast<u32>(mode.h));
        }
    };
    int count = 0;
    if (SDL_DisplayMode** const modes = SDL_GetFullscreenDisplayModes(monitor, &count)) {
        for (int i = 0; i < count; ++i) {
            add(*modes[i]);
        }
        SDL_free(modes);
    }
    if (const SDL_DisplayMode* const current = SDL_GetCurrentDisplayMode(monitor)) {
        add(*current);
    }
    std::vector<Size> sizes;
    for (const auto& [width, height] : found) {
        sizes.push_back({width, height});
    }
    std::scoped_lock lock{g_sizes_mutex};
    g_monitor_sizes = std::move(sizes);
}

bool SDLCALL WatchEvents(void*, SDL_Event* event) {
    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
        event->window.windowID == g_window_id.load(std::memory_order_relaxed)) {
        StoreSize(event->window.data1, event->window.data2);
    }
    // The window is on another monitor, or a monitor runs another mode.
    if (event->type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED ||
        (event->type == SDL_EVENT_WINDOW_DISPLAY_CHANGED &&
         event->window.windowID == g_window_id.load(std::memory_order_relaxed))) {
        StoreRefreshRate();
        StoreMonitorSizes();
    }
    // Shift + Print Screen + F2 stops the capture, and starts it again. The picture stands still
    // while it is stopped, so no key alone does it.
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_F2 && !event->key.repeat &&
        (event->key.mod & SDL_KMOD_SHIFT) != 0 && PrintScreenHeld()) {
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

void SDLCALL ShowOnMainThread(void*) {
    const Settings::Display display = Settings::Get().display;
    SDL_ShowWindow(g_window);
    if (display.fullscreen) {
        EnterFullscreen(ChosenDisplay(display), display);
    }
    // The mouse turns the camera: while the window has focus, the cursor is hidden and held. Touch
    // pad swipes need a free cursor instead.
    if (!Input::IsTouchpadSwipeEnabled() && !SDL_SetWindowRelativeMouseMode(g_window, true)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the window cannot hold the mouse: {}", SDL_GetError());
    }
    SDL_SyncWindow(g_window);
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    StoreSize(width, height);
    StoreRefreshRate();
    StoreMonitorSizes();
    g_shown = display;
}

void SDLCALL ApplyOnMainThread(void*) {
    const Settings::Display display = Settings::Get().display;
    if (display.fullscreen == g_shown.fullscreen && display.exclusive == g_shown.exclusive &&
        display.width == g_shown.width && display.height == g_shown.height) {
        return;
    }
    g_shown = display;
    if (display.fullscreen) {
        EnterFullscreen(SDL_GetDisplayForWindow(g_window), display);
        return;
    }
    if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0 &&
        !SDL_SetWindowFullscreen(g_window, false)) {
        LOG_ERROR(Render_Vulkan, "GR2port: no windowed mode: {}", SDL_GetError());
    }
    SDL_SetWindowSize(g_window, static_cast<int>(display.width), static_cast<int>(display.height));
}

// Main thread: the mouse's mode before a prompt freed the cursor, while the cursor is free.
Input::MouseMode g_freed_mode = Input::MouseMode::Off;

// While the cursor is free, the mouse turns neither the camera nor the stick.
void SDLCALL FreeCursorOnMainThread(void*) {
    g_freed_mode = Input::GetMouseMode();
    Input::SetMouseMode(Input::MouseMode::Off);
    SDL_SetWindowRelativeMouseMode(g_window, false);
}

void SDLCALL HoldCursorOnMainThread(void*) {
    Input::SetMouseMode(g_freed_mode);
    SDL_SetWindowRelativeMouseMode(g_window, !Input::IsTouchpadSwipeEnabled());
}

} // namespace

SDL_Window* Open() {
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
    // The swapchain takes the screen's own pixels on a scaled desktop. The window's size stays in
    // the desktop's units.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    // The renderer shows it with its first picture.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (g_window == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port: no window: {}", SDL_GetError());
        return nullptr;
    }
    int width = initial_width;
    int height = initial_height;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    StoreSize(width, height);
    g_window_id.store(SDL_GetWindowID(g_window), std::memory_order_relaxed);
    SDL_AddEventWatch(&WatchEvents, nullptr);
    return g_window;
}

SDL_Window* Show() {
    if (g_window == nullptr) {
        return nullptr;
    }
    if (!SDL_RunOnMainThread(&ShowOnMainThread, nullptr, true)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the main thread did not show the window: {}",
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

std::vector<Size> MonitorSizes() {
    std::scoped_lock lock{g_sizes_mutex};
    return g_monitor_sizes;
}

void Apply() {
    if (g_window != nullptr && !SDL_RunOnMainThread(&ApplyOnMainThread, nullptr, false)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the main thread did not take the display settings: {}",
                  SDL_GetError());
    }
}

void ToggleFullscreen() {
    if (g_window == nullptr) {
        return;
    }
    if ((SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0) {
        if (!SDL_SetWindowFullscreen(g_window, false)) {
            LOG_ERROR(Render_Vulkan, "GR2port: no windowed mode: {}", SDL_GetError());
        }
    } else {
        EnterFullscreen(SDL_GetDisplayForWindow(g_window), Settings::Get().display);
    }
}

void FreeCursor(bool free) {
    if (!SDL_RunOnMainThread(free ? &FreeCursorOnMainThread : &HoldCursorOnMainThread, nullptr,
                             false)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the main thread did not take the cursor: {}",
                  SDL_GetError());
    }
}

} // namespace Gr2Port::Window
