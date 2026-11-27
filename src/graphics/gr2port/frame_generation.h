// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "graphics/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// FSR 3.1's frame generation, on the render thread: from each two of the game's frames, the frame
// halfway between them, which the window shows half a frame before the later one. It moves the
// world's picture without the screen drawing and draws the later frame's screen drawing over it.
// FSR 3.1.6's runs where the device takes derivatives in compute shaders, else FSR 3.1.4's.
namespace Gr2Port::FrameGeneration {

/// After Upscaler::Start: with frame generation chosen and the GPU able to run it, makes the
/// context for the picture the game will name and render, the scene's size for it, before the game
/// starts, so every pipeline goes into the pipeline cache.
void Start(const Device& device, Memory& memory, vk::Extent2D render, vk::Extent2D picture);

/// Whether the context was made: the velocity pass then writes the motion it reads.
bool Made();

/// With the GPU idle, after Textures::Start: keeps the context for the same sizes, else makes it
/// again for a scene of render and a picture of picture, and names its picture
/// Scene::GeneratedHandle. depth views the scene's depth.
void Resize(vk::Extent2D render, vk::Extent2D picture, vk::ImageView depth);

/// Render thread, before the frame's images are acquired: whether the frame takes a generated
/// present before its own. The screen must refresh at least twice for each of the game's frames,
/// and the window's present must be SDR.
bool Presents(bool hdr);

/// Outside rendering, after the composite drew the world into the back buffer, picture of extent,
/// which rests in the shader read layout: copies it as the picture without the screen drawing.
void CopyHudless(vk::CommandBuffer cmd, vk::Image picture, vk::Extent2D extent);

/// Outside rendering, after the screen drawing: makes the frame between the last frame and this
/// one from the scene's depth (in the depth read layout) and motion (in the general layout) and
/// from picture, the back buffer, at the size Resize took. Frames up to completed are done on the
/// GPU. False when the generated frame is not to be shown: the history starts again, or this
/// frame's world was not copied. The generated picture is then sampled through
/// Scene::GeneratedHandle in the shader read layout.
bool Record(vk::CommandBuffer cmd, u64 frame, u64 completed, vk::Image depth, vk::Image motion,
            vk::Image picture, vk::Extent2D picture_extent);

/// Render thread, between a frame's two presents: waits until half the game's frame time has
/// passed since presented, the generated frame's present.
void WaitHalfFrame(std::chrono::steady_clock::time_point presented);

} // namespace Gr2Port::FrameGeneration
