// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"
#include "graphics/gr2port/settings.h"
#include "graphics/renderer_vulkan/vk_common.h"

namespace Gr2Port {
struct Device;
}

// The game's main thread reads the pad, simulates its frame and waits for its draw thread, which
// waits for the vblank to flip the frame before. A frame reaches the renderer two vblanks after
// its input. With low latency on, the main thread waits for the vblank before it reads the pad,
// each vblank flips every flip waiting, and the frame goes on to the renderer at once. Reflex and
// Anti-Lag 2 add their drivers' sleep after the vblank. Universal delays the frame's start by the
// time the last frames still waited for the draw thread.
namespace Gr2Port::LowLatency {

/// Render thread, once the device is made.
void Start(const Device& device);

/// Any thread: whether the GPU has mode.
bool Has(Settings::LowLatency mode);

/// Render thread, when it starts and when the settings change while the game runs: takes wanted,
/// or Universal where the GPU lacks it. Returns whether the swapchain must be made again.
bool Choose(Settings::LowLatency wanted);

/// Any thread: the mode Choose took.
Settings::LowLatency Mode();

/// Present thread, at a vblank that takes a flip: whether it takes every flip waiting.
bool FlipsAll();

/// Present thread, after a vblank took its flips: the frame waiting for it starts.
void OnFlipVblank();

/// The game's main thread, before it reads the pad.
void BeforeInput();

/// Any game thread, before and after it waits on semaphore. The main thread waits for its draw
/// thread on one of them.
void BeforeWait(u32 semaphore);
void AfterWait(u32 semaphore);

/// Draw thread, as the capture of the game's frame starts.
void Draws(u64 frame);

/// Render thread: the low latency frame of the game's frame, or 0 when it has none or was drawn.
u64 FrameOf(u64 frame);

/// Render thread: whether swapchains take Reflex.
bool ReflexSwapchain();

/// Render thread, after Create made swapchain, and before it replaces the one it made.
void SwapchainMade(vk::SwapchainKHR swapchain);
void SwapchainGone();

/// Render thread, after the submit of frame (a FrameOf value), and around its first present.
void Submitted(u64 frame);
void BeforePresent(u64 frame);
void AfterPresent(u64 frame);

/// Render thread: the mode and the frames' waits since the last report, or empty when off.
std::string Report(double seconds);

} // namespace Gr2Port::LowLatency
