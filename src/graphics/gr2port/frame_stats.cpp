// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <fmt/format.h>
#include <imgui.h>

#include "graphics/gr2port/frame_stats.h"

namespace Gr2Port::FrameStats {

namespace {

std::mutex g_mutex;
Values g_values;

// The outline is about a fourteenth of the font's height, at least a pixel.
void OutlinedText(const std::string& text) {
    ImDrawList* const list = ImGui::GetWindowDrawList();
    ImFont* const font = ImGui::GetFont();
    const float size = ImGui::GetFontSize();
    const float width = std::max(1.0f, std::round(size / 14.0f));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx != 0 || dy != 0) {
                list->AddText(font, size, ImVec2{at.x + dx * width, at.y + dy * width},
                              IM_COL32(0, 0, 0, 255), text.c_str());
            }
        }
    }
    list->AddText(font, size, at, IM_COL32(255, 255, 255, 255), text.c_str());
    ImGui::Dummy(ImGui::CalcTextSize(text.c_str()));
}

} // namespace

void Publish(const Values& values) {
    std::scoped_lock lock{g_mutex};
    g_values = values;
}

Values Get() {
    std::scoped_lock lock{g_mutex};
    return g_values;
}

void Draw(float presented_fps) {
    const Values values = Get();
    std::string line = fmt::format("FPS {:.0f} ({:.1f} ms)", presented_fps,
                                   presented_fps > 0.0f ? 1000.0f / presented_fps : 0.0f);
    if (values.game_fps > 0.0) {
        line += fmt::format(" | CPU FPS {:.0f}", values.game_fps);
    }
    if (!values.drawing) {
        line += fmt::format(" | GR2port off, GR2fork {}", values.fork ? "on" : "off");
    } else {
        if (values.gpu_ms > 0.0) {
            line += fmt::format(" | GPU FPS {:.0f}", 1000.0 / values.gpu_ms);
        }
        line += fmt::format(" | Render thread {:.2f} ms | Render thread wait {:.2f} ms | Capture "
                            "thread {:.2f} ms",
                            values.render_ms, values.wait_ms, values.capture_ms);
        if (values.fork) {
            line += " | GR2fork on";
        }
    }
    OutlinedText(line);
}

} // namespace Gr2Port::FrameStats
