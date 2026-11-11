// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

#include "graphics/gr2port/device.h"
#include "graphics/gr2port/scene.h"

namespace Gr2Port {
class Memory;
}

// The passes that give a frame GR2's look, on the render thread: the sky behind the scene, the
// meter that sets the exposure, the bloom, and the game's Tonemap, Antialias and Composite
// programs, which turn the scene's HDR colour into the picture.
//
// The scene's alpha stands for the game's stencil bit 3, which keeps a pixel in the antialias
// program's history: the world and the sky write 1, the passes that clear the bit write 0.
namespace Gr2Port::Look {

inline constexpr vk::Format SceneFormat = vk::Format::eR16G16B16A16Sfloat;

/// The game's glow blend, colour (constant, one) and alpha (one, zero): the glow draws add their
/// colour, the blend constant already in it, and write their alpha, which picks the blur's kernel.
inline constexpr vk::PipelineColorBlendAttachmentState GlowBlend{
    .blendEnable = true,
    .srcColorBlendFactor = vk::BlendFactor::eOne,
    .dstColorBlendFactor = vk::BlendFactor::eOne,
    .colorBlendOp = vk::BlendOp::eAdd,
    .srcAlphaBlendFactor = vk::BlendFactor::eOne,
    .dstAlphaBlendFactor = vk::BlendFactor::eZero,
    .alphaBlendOp = vk::BlendOp::eAdd,
    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
};

/// Inside the glow's rendering, at the bloom's extent: draws the characters' glow.
using GlowDraws = std::function<void(vk::CommandBuffer cmd, vk::Extent2D extent)>;

/// After Textures::Start: creates the pipelines and the meter. depth_format is the scene's.
bool Start(const Device& device, Memory& memory, vk::Format depth_format);

/// With the GPU idle: creates the scene's colour image at extent, the bloom's images, the
/// antialias images at output, and the picture's pipeline for picture_format. depth is the scene's
/// depth, for the bloom to read. output differs from extent only with an upscaler, after its
/// Resize.
bool Resize(vk::Extent2D extent, vk::Extent2D output, vk::Format picture_format,
            vk::ImageView depth);

vk::Image SceneImage();
vk::ImageView SceneView();

/// Inside the scene's rendering, after its opaque draws: draws the sky where nothing is drawn yet.
void RecordSky(vk::CommandBuffer cmd, u64 packet_address);

/// After the scene's rendering, with the scene's colour readable: measures it and sets the
/// exposure.
void RecordMeter(vk::CommandBuffer cmd, u64 packet_address);

/// Outside rendering, with the scene's colour and depth readable: draws the game's bloom when the
/// header's bloom flags say it runs, with the glow draws of the packet at packet_address, which is
/// at offset in buffer, and the characters' glow by character_glow.
void RecordBloom(vk::CommandBuffer cmd, const Scene::Header& header, u64 packet_address,
                 vk::Buffer buffer, u64 offset, const GlowDraws& character_glow);

/// Outside rendering, with the scene's colour readable, after the bloom: tonemaps the scene and
/// runs the game's antialias program on the result.
void RecordAntialias(vk::CommandBuffer cmd, u64 packet_address);

/// Inside the picture's rendering, after RecordAntialias: draws the picture, which fills extent.
void RecordComposite(vk::CommandBuffer cmd, u64 packet_address, vk::Extent2D extent);

/// Outside rendering, once per report: logs the frame's look and the meter's last read state, and
/// reads the state again.
void LogAndReadMeter(vk::CommandBuffer cmd, const u8* packet);

} // namespace Gr2Port::Look
