// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <utility>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include "common/logging/log.h"
#include "video_core/gr2_pt/window.h"

namespace Gr2Pt::Window {

namespace {

constexpr int InitialWidth = 1280;
constexpr int InitialHeight = 720;

SDL_Window* g_window = nullptr;
std::atomic<SDL_WindowID> g_window_id{0};
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
    return true;
}

void SDLCALL CreateOnMainThread(void*) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* const game = count > 0 ? windows[0] : nullptr;
    SDL_free(windows);

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "GR2 PT");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, InitialWidth);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, InitialHeight);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    if (game != nullptr) {
        SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_PARENT_POINTER, game);
    }
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (g_window == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no window: {}", SDL_GetError());
        return;
    }
    int width = InitialWidth;
    int height = InitialHeight;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    StoreSize(width, height);
    g_window_id.store(SDL_GetWindowID(g_window), std::memory_order_relaxed);
    SDL_AddEventWatch(&WatchEvents, nullptr);
}

void SDLCALL SetTitleOnMainThread(void* data) {
    const auto* title = static_cast<std::string*>(data);
    if (g_window != nullptr) {
        SDL_SetWindowTitle(g_window, title->c_str());
    }
    delete title;
}

} // namespace

SDL_Window* Create() {
    if (!SDL_RunOnMainThread(&CreateOnMainThread, nullptr, true)) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: the main thread did not create the window: {}",
                  SDL_GetError());
        return nullptr;
    }
    return g_window;
}

Size PixelSize() {
    const u64 size = g_size.load(std::memory_order_relaxed);
    return {static_cast<u32>(size >> 32), static_cast<u32>(size)};
}

void SetTitle(std::string title) {
    auto* data = new std::string(std::move(title));
    if (!SDL_RunOnMainThread(&SetTitleOnMainThread, data, false)) {
        delete data;
    }
}

} // namespace Gr2Pt::Window
