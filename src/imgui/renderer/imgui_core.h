// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <imgui.h>

#include "common/types.h"

union SDL_Event;

namespace Frontend {
class WindowSDL;
}

namespace ImGui::Core {

void Initialize(const Frontend::WindowSDL& window);

void Shutdown();

bool ProcessEvent(SDL_Event* event);

void AcquireGamepadInputCapture();
void ReleaseGamepadInputCapture();
bool IsGamepadInputCaptured();

ImGuiID NewFrame(bool is_reusing_frame = false);

/// Ends ImGui's frame and hands its draw lists and texture requests to GR2port's renderer.
void Render();

bool MustKeepDrawing(); // Force the emulator redraw

} // namespace ImGui::Core
