// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "common/debug_files.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "font_data.h"
#include "font_stack.h"
#include "imgui/imgui_layer.h"
#include "imgui_core.h"
#include "imgui_impl_sdl3.h"
#include "imgui_internal.h"
#include "sdl_window.h"
#include "texture_manager.h"

#include "graphics/gr2port/imgui_relay.h"

// The font atlas grows to this size at most, which every GPU the renderer runs on can make.
constexpr int AtlasMaxSize = 8192;
// ImGui lays its windows out on a canvas of this size, which the renderer scales to fit the
// window and centres.
constexpr ImVec2 CanvasSize{1280.0f, 720.0f};

static std::vector<ImGui::Layer*> layers;

// Update layers before rendering to allow layer changes to be applied during rendering.
// Using deque to keep the order of changes in case a Layer is removed then added again between
// frames.
static std::deque<std::pair<bool, ImGui::Layer*>> change_layers{};
static std::mutex change_layers_mutex{};

static ImGuiID dock_id;
static std::atomic<std::uint32_t> force_gamepad_input_capture_count{0};

// ImGui's context is not thread-safe: the window thread queues its events here, and the present
// thread gives them to ImGui as its frame starts. A text event's text lives in SDL's memory only
// until the next event, so it is copied.
struct QueuedEvent {
    SDL_Event event;
    std::string text;
};
static std::mutex queued_events_mutex;
static std::vector<QueuedEvent> queued_events;
static std::vector<QueuedEvent> taken_events;

// What ImGui wanted at the end of its last frame, which ProcessEvent decides by.
static std::atomic<bool> wants_mouse{false};
static std::atomic<bool> wants_keyboard{false};
static std::atomic<bool> wants_pad{false};

namespace ImGui {

namespace Core {

void AcquireGamepadInputCapture() {
    force_gamepad_input_capture_count.fetch_add(1, std::memory_order_relaxed);
}

void ReleaseGamepadInputCapture() {
    std::uint32_t expected = force_gamepad_input_capture_count.load(std::memory_order_relaxed);
    while (expected != 0) {
        if (force_gamepad_input_capture_count.compare_exchange_weak(
                expected, expected - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
    LOG_WARNING(ImGui, "ReleaseGamepadInputCapture called with no active capture");
}

bool IsGamepadInputCaptured() {
    return force_gamepad_input_capture_count.load(std::memory_order_relaxed) > 0;
}

void Initialize(const Frontend::WindowSDL& window) {

    const auto config_path = GetUserPath(Common::FS::PathType::UserDir) / "imgui.ini";
    const auto log_path = GetUserPath(Common::FS::PathType::LogDir) / "imgui_log.txt";

    CreateContext();
    ImGuiIO& io = GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = CanvasSize;
    PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); // Makes the window edges rounded

    auto path = config_path.u8string();
    char* config_file_buf = new char[path.size() + 1]();
    std::memcpy(config_file_buf, path.c_str(), path.size());
    io.IniFilename = config_file_buf;

    // GR2port: ImGui's own log is one of the developers' files.
    io.LogFilename = nullptr;
    if (Common::DebugFiles()) {
        path = log_path.u8string();
        char* log_file_buf = new char[path.size() + 1]();
        std::memcpy(log_file_buf, path.c_str(), path.size());
        io.LogFilename = log_file_buf;
    }

    ImFontConfig font_cfg{};
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    const int console_language = EmulatorSettings.GetConsoleLanguage();
    io.FontDefault = FontStack::AddPrimaryUiFont(io.Fonts, 32.0f, console_language, font_cfg, true);

    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_proggyvector_regular_compressed_data,
                                             imgui_font_proggyvector_regular_compressed_size,
                                             32.0f);

    io.Fonts->TexMaxWidth = AtlasMaxSize;
    io.Fonts->TexMaxHeight = AtlasMaxSize;

    io.FontGlobalScale = 0.5f;

    StyleColorsDark();

    ::Core::Devtools::Layer::SetupSettings();
    Sdl::Init(window.GetSDLWindow());

    // GR2port's renderer draws the frames: it takes vertex offsets and ImGui's texture requests.
    io.BackendRendererName = "GR2port";
    io.BackendFlags |=
        ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

    TextureManager::StartWorker();

    char label[32];
    ImFormatString(label, IM_ARRAYSIZE(label), "WindowOverViewport_%08X", GetMainViewport()->ID);
    dock_id = ImHashStr(label);

    std::at_quick_exit([] { SaveIniSettingsToDisk(GetIO().IniFilename); });
}

void Shutdown() {
    TextureManager::StopWorker();

    ImGuiIO& io = GetIO();
    const auto ini_filename = (void*)io.IniFilename;
    const auto log_filename = (void*)io.LogFilename;

    // ImGui's own textures end with the backend that took their requests.
    for (ImTextureData* texture : GetPlatformIO().Textures) {
        if (texture->RefCount == 1) {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
    }
    io.BackendRendererName = nullptr;
    io.BackendFlags &=
        ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    Sdl::Shutdown();
    DestroyContext();

    delete[] (char*)ini_filename;
    delete[] (char*)log_filename;
}

bool ProcessEvent(SDL_Event* event) {
    SDL_Event relayed;
    const SDL_Event* const mapped = Gr2Port::ImGuiRelay::ForImGui(event, relayed);
    {
        std::scoped_lock lock{queued_events_mutex};
        // ImGui takes the last position of moves that follow each other.
        if (mapped->type == SDL_EVENT_MOUSE_MOTION && !queued_events.empty() &&
            queued_events.back().event.type == SDL_EVENT_MOUSE_MOTION) {
            queued_events.back().event = *mapped;
        } else {
            const bool text = mapped->type == SDL_EVENT_TEXT_INPUT && mapped->text.text != nullptr;
            queued_events.push_back({*mapped, text ? mapped->text.text : ""});
        }
    }
    switch (event->type) {
    // Don't block release/up events
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return wants_mouse.load(std::memory_order_relaxed);
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN: {
        if (IsGamepadInputCaptured()) {
            // Keep keyboard events flowing through the regular input-binding path while IME/OSK
            // captures gamepad input, so keyboard equivalents of pad buttons still update the
            // virtual controller state.
            return false;
        }
        return wants_keyboard.load(std::memory_order_relaxed);
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        if (IsGamepadInputCaptured()) {
            // Let controller events continue to input bindings so OSK shortcuts and virtual
            // controller state keep updating; game-side pad reads are blocked in libScePad.
            return false;
        }
        return wants_pad.load(std::memory_order_relaxed);
    }
    default:
        return false;
    }
}

ImGuiID NewFrame(bool is_reusing_frame) {
    {
        std::scoped_lock lock{change_layers_mutex};
        while (!change_layers.empty()) {
            const auto [to_be_added, layer] = change_layers.front();
            if (to_be_added) {
                layers.push_back(layer);
            } else {
                const auto [begin, end] = std::ranges::remove(layers, layer);
                layers.erase(begin, end);
            }
            change_layers.pop_front();
        }
    }

    {
        std::scoped_lock lock{queued_events_mutex};
        taken_events.swap(queued_events);
    }
    for (QueuedEvent& queued : taken_events) {
        if (queued.event.type == SDL_EVENT_TEXT_INPUT) {
            queued.event.text.text = queued.text.c_str();
        }
        Sdl::ProcessEvent(&queued.event);
    }
    taken_events.clear();

    Sdl::NewFrame(is_reusing_frame);
    ImGui::NewFrame();
    SetKeyOwner(ImGuiKey_GamepadFaceUp, ImHashStr("gr2port/pad"));

    ImGuiWindowFlags flags =
        ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
    if (!DebugState.IsShowingDebugMenuBar()) {
        flags |= ImGuiDockNodeFlags_NoTabBar;
    }
    ImGuiID dockId = DockSpaceOverViewport(0, GetMainViewport(), flags);

    for (auto* layer : layers) {
        layer->Draw();
    }

    return dockId;
}

void Render() {
    ImGui::Render();
    TextureManager::Submit();
    Gr2Port::ImGuiRelay::Capture(*GetDrawData());

    const ImGuiIO& io = GetIO();
    const ImGuiWindow* const nav = io.Ctx->NavWindow;
    wants_mouse.store(io.WantCaptureMouse && nav != nullptr &&
                          (nav->Flags & ImGuiWindowFlags_NoNav) == 0,
                      std::memory_order_relaxed);
    wants_keyboard.store(io.WantCaptureKeyboard && nav != nullptr && nav->ID != dock_id,
                         std::memory_order_relaxed);
    wants_pad.store(io.NavActive && nav != nullptr && nav->ID != dock_id,
                    std::memory_order_relaxed);
}

bool MustKeepDrawing() {
    return std::ranges::any_of(layers, [](Layer* layer) { return layer->ShouldKeepDrawing(); }) ||
           change_layers.size() > 1;
}

} // namespace Core

void Layer::AddLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(true, layer);
}

void Layer::RemoveLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(false, layer);
}

} // namespace ImGui
