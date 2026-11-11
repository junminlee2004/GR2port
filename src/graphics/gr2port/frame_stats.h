// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// GR2port's frame statistics for the F10 counter: the renderer publishes them twice a second, and
// the game's frame capture while GR2port does not draw.
namespace Gr2Port::FrameStats {

struct Values {
    /// GR2port draws the game; the timings below are its own.
    bool drawing = false;
    /// GR2fork's GPU emulation runs beside GR2port.
    bool fork = false;
    /// Game frames per second, as the game's own threads finish them.
    double game_fps = 0.0;
    /// GPU time per drawn frame, render thread work per drawn frame, its wait for a frame slot
    /// and a swapchain image, and the capture on the game's draw thread per game frame.
    double gpu_ms = 0.0;
    double render_ms = 0.0;
    double wait_ms = 0.0;
    double capture_ms = 0.0;
};

/// Any thread.
void Publish(const Values& values);

/// Any thread: the values published last.
Values Get();

/// In an ImGui window: the counter on one line, white with a black outline, from the presented
/// frame rate and the values published last.
void Draw(float presented_fps);

} // namespace Gr2Port::FrameStats
