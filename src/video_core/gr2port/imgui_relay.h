// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"

struct ImDrawData;
union SDL_Event;

namespace ImGui {
struct Texture;
}

namespace Gr2Port {
class Memory;
}

// The emulator's ImGui windows (save and load prompts, messages, notices) over GR2port's picture
// while GR2fork's window does not show them. Each ImGui frame's draw lists are copied on the
// emulator's present thread, with the pixels of the textures they sample, and the renderer draws
// them last, scaled from the emulator's window to its own. The game's display window stays out.
namespace Gr2Port::ImGuiRelay {

/// Present thread, after ImGui::Render and before the backend takes the texture requests.
void Capture(const ImDrawData& draw_data);

/// Any thread: the RGBA8 pixels of a texture the emulator uploads itself, width * height of them.
void KeepPixels(const ImGui::Texture* texture, const void* pixels, u32 width, u32 height);

/// Any thread: the emulator's texture is gone.
void Forget(const ImGui::Texture* texture);

/// Main thread, for the ImGui backend: while the windows show on GR2port's window, its mouse,
/// key and focus events as the game window's, the mouse in the game window's coordinates, in
/// copy. Other events come back as they are.
const SDL_Event* ForImGui(const SDL_Event* event, SDL_Event& copy);

/// ImGui's frame, as it hides the idle cursor: when the mouse is over GR2port's window, puts it
/// back where it is there to show the change, and returns true.
bool WarpBack();

/// Render thread, after Textures::Start.
bool Start(const Device& device, Memory& memory);

/// Render thread: the pipeline for the window's format.
bool Resize(vk::Format window_format);

/// Render thread, before the frame's last rendering: uploads the textures the latest ImGui frame
/// samples, and its vertices. Frames up to completed are done on the GPU.
void Update(vk::CommandBuffer cmd, u64 frame, u64 completed);

/// Render thread, inside the frame's last rendering, over the picture.
void Record(vk::CommandBuffer cmd, vk::Extent2D extent);

} // namespace Gr2Port::ImGuiRelay
