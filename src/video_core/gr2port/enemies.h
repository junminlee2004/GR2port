// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

// opaque/G2_EN: the enemies' bodies and cores, drawn with the game's own programs.
namespace Gr2Port::Enemies {

/// After Textures::Start and SelfTranslucent::Start: starts building the pipelines on their own
/// thread. The draws stay off until they are ready.
bool Start(const Device& device, vk::Format scene_format, vk::Format depth_format);

/// Inside the characters' depth rendering: draws the enemies' depth from the packet at address,
/// which is at offset in buffer.
void RecordDepth(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
                 u64 offset, vk::Extent2D extent);

/// Whether the packet holds cores that SelfTranslucent::Record draws.
bool HasSelfTranslucent(const Scene::Header& header);

/// Inside SelfTranslucent's rendering, after the effects inside the skin: draws the cores into
/// its targets.
void RecordSelfTranslucent(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                           vk::Buffer buffer, u64 offset, vk::Extent2D extent);

/// Inside the forward rendering into the scene's colour and depth, after SelfTranslucent::Record:
/// draws the enemies.
void RecordForward(vk::CommandBuffer cmd, const Scene::Header& header, u64 address,
                   vk::Buffer buffer, u64 offset, vk::Extent2D extent);

/// Inside the forward rendering, after the sky: draws the cores' alpha pass, G_BG_ENCore.
void RecordAlpha(vk::CommandBuffer cmd, const Scene::Header& header, u64 address, vk::Buffer buffer,
                 u64 offset, vk::Extent2D extent);

} // namespace Gr2Port::Enemies
