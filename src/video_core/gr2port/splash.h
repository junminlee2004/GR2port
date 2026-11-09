// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
class Swapchain;
} // namespace Gr2Port

// The game's boot picture, sce_sys/pic1.png, which the PS4 shows while a game starts. The window
// shows it from the start until the renderer's first frame replaces it.
namespace Gr2Port::Splash {

/// Render thread, with the device idle: creates the swapchain at the window's size and presents
/// the picture, fitted to the window over black. Does nothing when the game has no picture.
void Show(const Device& d, Memory& memory, Swapchain& swapchain, vk::CommandPool pool,
          vk::CommandBuffer cmd, vk::Fence done, vk::Semaphore acquired);

} // namespace Gr2Port::Splash
