// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
struct Image;
} // namespace Gr2Port

// G2_EF_Main: the game's effect meshes, drawn with its own programs from its sorted lists.
namespace Gr2Port::Effects {

/// After Textures::Start: creates the set and starts building the pipelines on their own thread,
/// since the programs are large. The draws stay off until they are ready.
bool Start(const Device& device, vk::Format scene_format, vk::Format depth_format);

/// The game's blend state for an item's blend mode, below 8, which G2_EF_Main and G_CH_Glass
/// items share.
vk::PipelineColorBlendAttachmentState BlendState(u32 mode);

/// With the GPU idle: points the programs at the scene's depth and at the copy of its colour.
void Resize(vk::ImageView depth, vk::ImageView background);

/// Inside SelfTranslucent's rendering, with its viewport set: draws the game's first two lists of
/// the packet at address, which is at offset in buffer.
void RecordHidden(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet, u64 address,
                  vk::Buffer buffer, u64 offset);

/// Outside rendering, with the scene's colour and depth as attachments: draws the game's
/// TransparentBg list, and leaves them as attachments.
void RecordTransparentBg(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                         u64 address, vk::Buffer buffer, u64 offset, vk::ImageView scene,
                         const Image& depth, vk::Extent2D extent);

/// Outside rendering, with the scene's colour an attachment and depth readable: draws the game's
/// TransparentFg list.
void RecordTransparentFg(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                         u64 address, vk::Buffer buffer, u64 offset, vk::ImageView scene,
                         vk::ImageView depth, vk::Extent2D extent);

/// Outside rendering, with the scene's colour an attachment and depth readable: draws the game's
/// TransparentTopMost list.
void RecordTopMost(vk::CommandBuffer cmd, const Scene::Header& header, const u8* packet,
                   u64 address, vk::Buffer buffer, u64 offset, vk::ImageView scene,
                   vk::ImageView depth, vk::Extent2D extent);

} // namespace Gr2Port::Effects
