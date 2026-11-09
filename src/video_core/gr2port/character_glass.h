// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

// G_CH_Glass, category 12: the glass on people and props, drawn with the game's own programs.
namespace Gr2Port::CharacterGlass {

/// After Textures::Start and Shadows::Start: starts building the pipelines on their own thread.
/// The draws stay off until they are ready.
bool Start(const Device& device, vk::Format scene_format, vk::Format depth_format);

/// Inside the forward rendering into the scene's colour and depth, after the sky: draws the
/// packet's G_CH_Glass draws in the game's order. The packet is at address and at offset in
/// buffer; data is its memory.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, const u8* data, u64 address,
            vk::Buffer buffer, u64 offset, vk::Extent2D extent);

} // namespace Gr2Port::CharacterGlass
