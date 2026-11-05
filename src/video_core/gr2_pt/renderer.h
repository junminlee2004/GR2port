// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The render thread: draws a frame into the renderer's window for each frame the game builds.
namespace Gr2Pt::Renderer {

/// Starts the render thread, which creates the window and the device.
void Start();

/// The game's draw thread starts a frame. Never blocks.
void OnGameFrame();

} // namespace Gr2Pt::Renderer
