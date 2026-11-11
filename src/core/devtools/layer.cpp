// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include "layer.h"

#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "SDL3/SDL_log.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "imgui/imgui_std.h"
#include "imgui_internal.h"

#include "graphics/gr2port/frame_stats.h"

#include "widget/frame_graph.h"
#include "widget/gpu_memory.h"
#include "widget/memory_map.h"
#include "widget/module_list.h"

using namespace ImGui;
using namespace ::Core::Devtools;
using L = ::Core::Devtools::Layer;

static bool show_simple_fps = false;
static bool visibility_toggled = false;
static bool fps_pinned = false;
static bool show_quit_window = false;

static bool show_volume = false;
static float volume_start_time;

static Widget::FrameGraph frame_graph;

static float debug_popup_timing = 3.0f;

static Widget::MemoryMapViewer memory_map;
static Widget::ModuleList module_list;
static Widget::GpuMemoryViewer gpu_memory;

void L::DrawMenuBar() {
    const auto& ctx = *GImGui;
    const auto& io = ctx.IO;

    auto isSystemPaused = DebugState.IsGuestThreadsPaused();

    if (BeginMainMenuBar()) {
        if (BeginMenu("Options")) {
            if (MenuItemEx("Emulator Paused", nullptr, nullptr, isSystemPaused)) {
                if (isSystemPaused) {
                    DebugState.ResumeGuestThreads();
                } else {
                    DebugState.PauseGuestThreads();
                }
            }
            ImGui::EndMenu();
        }
        if (BeginMenu("GPU Tools")) {
            MenuItem("Show frame info", nullptr, &frame_graph.is_open);
            MenuItem("Show GPU memory usage", nullptr, &gpu_memory.open);
            ImGui::EndMenu();
        }
        if (BeginMenu("Debug")) {
            MenuItem("Memory map", nullptr, &memory_map.open);
            MenuItem("Module list", nullptr, &module_list.open);
            ImGui::EndMenu();
        }

        SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (Button("X", ImVec2(25, 25))) {
            DebugState.IsShowingDebugMenuBar() = false;
        }

        EndMainMenuBar();
    }
}

void L::DrawAdvanced() {
    DrawMenuBar();

    const auto& ctx = *GImGui;
    const auto& io = ctx.IO;

    frame_graph.Draw();

    if (!DebugState.debug_message_popup.empty()) {
        if (debug_popup_timing > 0.0f) {
            debug_popup_timing -= io.DeltaTime;
            if (Begin("##devtools_msg", nullptr,
                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove)) {
                BringWindowToDisplayFront(GetCurrentWindow());
                const auto display_size = io.DisplaySize;
                const auto& msg = DebugState.debug_message_popup.front();
                const auto padding = GetStyle().WindowPadding;
                const auto txt_size = CalcTextSize(&msg.front(), &msg.back() + 1, false, 250.0f);
                SetWindowPos({display_size.x - padding.x * 2.0f - txt_size.x, 50.0f});
                SetWindowSize({txt_size.x + padding.x * 2.0f, txt_size.y + padding.y * 2.0f});
                PushTextWrapPos(250.0f);
                TextEx(&msg.front(), &msg.back() + 1);
                PopTextWrapPos();
            }
            End();
        } else {
            DebugState.debug_message_popup.pop();
            debug_popup_timing = 3.0f;
        }
    }

    if (memory_map.open) {
        memory_map.Draw();
    }
    if (module_list.open) {
        module_list.Draw();
    }
    if (gpu_memory.open) {
        gpu_memory.Draw();
    }
}

void L::DrawSimple() {
    // GR2port: the frame rates and GR2port's timings, white with a black outline.
    Gr2Port::FrameStats::Draw(DebugState.Framerate);
}

static void LoadSettings(const char* line) {
    int i;
    if (sscanf(line, "show_advanced_debug=%d", &i) == 1) {
        // The debug menu bar starts hidden at every boot.
        return;
    }
    if (sscanf(line, "show_frame_graph=%d", &i) == 1) {
        frame_graph.is_open = i != 0;
        return;
    }
    if (sscanf(line, "show_memory_map=%d", &i) == 1) {
        memory_map.open = i != 0;
        return;
    }
    if (sscanf(line, "show_module_list=%d", &i) == 1) {
        module_list.open = i != 0;
        return;
    }
    if (sscanf(line, "show_gpu_memory=%d", &i) == 1) {
        gpu_memory.open = i != 0;
        return;
    }
}

void L::SetupSettings() {
    frame_graph.is_open = true;
    show_simple_fps = EmulatorSettings.IsShowFpsCounter();
    visibility_toggled = show_simple_fps;

    using SettingLoader = void (*)(const char*);

    ImGuiSettingsHandler handler{};
    handler.TypeName = "DevtoolsLayer";
    handler.TypeHash = ImHashStr(handler.TypeName);
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
        if (std::string_view("Data") == name) {
            static_assert(std::is_same_v<decltype(&LoadSettings), SettingLoader>);
            return (void*)&LoadSettings;
        }
        return (void*)nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* handle, const char* line) {
        if (handle != nullptr) {
            reinterpret_cast<SettingLoader>(handle)(line);
        }
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* handler, ImGuiTextBuffer* buf) {
        buf->appendf("[%s][Data]\n", handler->TypeName);
        buf->appendf("show_advanced_debug=%d\n", DebugState.IsShowingDebugMenuBar());
        buf->appendf("show_frame_graph=%d\n", frame_graph.is_open);
        buf->appendf("show_memory_map=%d\n", memory_map.open);
        buf->appendf("show_module_list=%d\n", module_list.open);
        buf->appendf("show_gpu_memory=%d\n", gpu_memory.open);
        buf->append("\n");
    };
    AddSettingsHandler(&handler);
}

bool L::ShouldKeepDrawing() {
    return DebugState.IsShowingDebugMenuBar();
}

void L::Draw() {
    const auto io = GetIO();
    PushID("DevtoolsLayer");

    if (!DebugState.IsGuestThreadsPaused()) {
        const auto fn = DebugState.flip_frame_count.load();
        frame_graph.AddFrame(fn, DebugState.FrameDeltaTime);
    }

    if (IsKeyPressed(ImGuiKey_F10, false)) {
        if (io.KeyCtrl) {
            DebugState.IsShowingDebugMenuBar() ^= true;
        }
        visibility_toggled = true;
    }

    if (DebugState.IsGuestThreadsPaused()) {
        ImVec2 pos = ImVec2(10, 10);
        ImU32 color = IM_COL32(255, 255, 255, 255);
        ImGui::GetForegroundDrawList()->AddText(pos, color, "Emulation Paused");
    }

    if (show_simple_fps) {
        if (Begin("Video Info", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking |
                      ImGuiWindowFlags_NoBackground)) {
            if (visibility_toggled) {
                fps_pinned = true;
                visibility_toggled = false;
            }
            if (GetCurrentContext()->MovingWindow == GetCurrentWindowRead() &&
                IsMouseDragging(ImGuiMouseButton_Left)) {
                fps_pinned = false;
            }
            if (fps_pinned) {
                SetWindowPos({GetIO().DisplaySize.x - GetCurrentWindowRead()->SizeFull.x, 0.0f});
            }
            KeepWindowInside();
            DrawSimple();
        }
        End();
    }

    if (DebugState.IsShowingDebugMenuBar()) {
        PushFont(io.Fonts->Fonts[IMGUI_FONT_MONO]);
        PushID("DevtoolsLayer");
        DrawAdvanced();
        PopID();
        PopFont();
    }

    if (show_quit_window) {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (Begin("Quit Notification", nullptr,
                  ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
            SetWindowFontScale(1.5f);
            Overlay::TextCentered("Are you sure you want to quit?");
            NewLine();
            Text("Press Escape or Circle/B button to cancel");
            Text("Press Enter or Cross/A button to quit");

            if (IsKeyPressed(ImGuiKey_Escape, false) ||
                (IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
                show_quit_window = false;
            }

            if (IsKeyPressed(ImGuiKey_Enter, false) ||
                (IsKeyPressed(ImGuiKey_GamepadFaceDown, false))) {
                SDL_Event event;
                SDL_memset(&event, 0, sizeof(event));
                event.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&event);
            }
        }
        End();
    }

    if (show_volume) {
        float current_time = ImGui::GetTime();

        // Show volume for 3 seconds
        if (current_time - volume_start_time >= 3.0) {
            show_volume = false;
        } else {
            SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x +
                                        ImGui::GetMainViewport()->WorkSize.x - 10,
                                    ImGui::GetMainViewport()->WorkPos.y + 10),
                             ImGuiCond_Always, ImVec2(1.0f, 0.0f));

            if (ImGui::Begin("Volume Window", &show_volume,
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
                Text("Volume: %d", EmulatorSettings.GetVolumeSlider());
            }
            End();
        }
    }

    PopID();
}

namespace Overlay {

void TextCentered(const std::string& text) {
    float window_width = GetContentRegionAvail().x;
    float text_width = CalcTextSize(text.c_str()).x;
    float text_indentation = (window_width - text_width) * 0.5f;

    SetCursorPosX(text_indentation);
    Text("%s", text.c_str());
}

void ToggleSimpleFps() {
    show_simple_fps = !show_simple_fps;
    visibility_toggled = true;
}

void SetSimpleFps(bool enabled) {
    show_simple_fps = enabled;
    visibility_toggled = true;
}

void ToggleQuitWindow() {
    show_quit_window = !show_quit_window;
}

void ShowVolume() {
    volume_start_time = ImGui::GetTime();
    show_volume = true;
}

} // namespace Overlay
