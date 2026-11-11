// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "graphics/gr2port/presenter.h"
#include "imgui/imgui_layer.h"
#include "imgui/renderer/imgui_core.h"

namespace Gr2Port {

Presenter::Presenter(Frontend::WindowSDL& window) {
    for (Frame& frame : frames) {
        free_queue.push(&frame);
    }
    ImGui::Core::Initialize(window);
    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
}

Presenter::~Presenter() {
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
    ImGui::Core::Shutdown();
}

Frame* Presenter::PrepareBlankFrame(bool) {
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] { return !free_queue.empty(); });
    Frame* const frame = free_queue.front();
    free_queue.pop();
    return frame;
}

Frame* Presenter::PrepareLastFrame() {
    return last_frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // ImGui's windows, and the prompts among them, reach the renderer from here.
    ImGui::Core::NewFrame(is_reusing_frame);
    ImGui::Core::Render();
    if (!is_reusing_frame) {
        last_frame = frame;
        std::scoped_lock lock{free_mutex};
        free_queue.push(frame);
        free_cv.notify_one();
    }
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

} // namespace Gr2Port
