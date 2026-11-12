// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"

namespace Gr2Port {
class Memory;
class Swapchain;
} // namespace Gr2Port

// The game's boot picture, sce_sys/pic1.png, which the PS4 shows while a game starts. The window
// shows it from the start until the renderer's first frame replaces it. While the pipelines are
// built, white text in the window's top left corner tells what part of them is made. A thread of
// its own presents the picture and the text several times a second with copies alone, so a
// pipeline the driver takes minutes over holds neither back.
namespace Gr2Port::Splash {

/// Render thread, with the device idle: creates the swapchain at the window's size, presents the
/// picture, fitted to the window over black, and starts the thread that keeps presenting it.
/// Until End, that thread alone uses the queue, the swapchain and the objects given here.
void Show(const Device& d, Memory& memory, Swapchain& swapchain, vk::CommandPool pool,
          vk::CommandBuffer cmd, vk::Fence done, vk::Semaphore acquired);

/// Render thread, before it draws its first frame or gives up: stops the thread and frees the
/// picture. done is signalled when it returns. Does nothing when the splash is not shown.
void End();

} // namespace Gr2Port::Splash
