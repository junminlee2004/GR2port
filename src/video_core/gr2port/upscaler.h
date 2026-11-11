// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "video_core/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// The temporal upscaler that the Graphics settings choose in place of the game's antialiasing, on
// the render thread. With one running, the scene draws at the render size, each frame's scene
// draws move by a sub-pixel jitter, and the upscaler turns the toned colour, the depth and the
// velocity into the picture at the output size.
namespace Gr2Port::Upscaler {

/// Sub-pixel offset of the frame's scene draws, in render pixels: +x right, +y down.
struct Offset {
    float x = 0.0f;
    float y = 0.0f;
};

/// While the instance's extensions are chosen: with DLSS chosen, starts Streamline and appends the
/// instance extensions DLSS needs.
void ExtendInstance(std::vector<const char*>& extensions,
                    const vk::detail::DispatchLoaderDynamic& dispatch);

/// Right before the device is made from info: enables the features and extensions the chosen
/// upscaler needs that gpu supports. features chains vk11, vk12 and vk13.
void ExtendDevice(vk::PhysicalDevice gpu, const vk::detail::DispatchLoaderDynamic& dispatch,
                  vk::DeviceCreateInfo& info, vk::PhysicalDeviceFeatures2& features,
                  vk::PhysicalDeviceVulkan11Features& vk11,
                  vk::PhysicalDeviceVulkan12Features& vk12,
                  vk::PhysicalDeviceVulkan13Features& vk13);

/// Right after the device and its dispatch are made: with DLSS chosen, hands the device to
/// Streamline, which then presents.
void OnDeviceCreated(Device& device);

/// After the device is made, before the textures start: checks that the device runs the chosen
/// upscaler, else the game's antialiasing runs.
bool Start(const Device& device, Memory& memory);

/// With the GPU idle: frees the context and the images.
void Stop();

/// Whether an upscaler replaces the game's antialiasing.
bool Active();

/// Any thread: the scene's size for a picture of output.
vk::Extent2D RenderExtent(vk::Extent2D output);

/// The mip level bias of the scene's material samplers: log2 of the render width over the output
/// width minus one, or 0 without an upscaler.
float LodBias();

/// With the GPU idle: makes the context and the output image for the sizes. When the context
/// cannot be made, the game's antialiasing runs and RenderExtent gives output. False when the
/// output image is not made.
bool Resize(vk::Extent2D render, vk::Extent2D output);

/// With the GPU idle, after Resize: the scene's depth and velocity at the render size.
void SetInputs(vk::Image depth, vk::ImageView depth_view, vk::Image velocity,
               vk::ImageView velocity_view);

/// With the GPU idle, after SetInputs: the toned colour the upscaler reads, at the render size.
void SetColor(vk::Image toned, vk::ImageView view, vk::Format format);

/// At the start of a frame that draws the scene: picks the frame's jitter. packet is the frame's
/// scene packet, which gives the camera; the renderer's frames up to frame - frames_in_flight
/// have finished on the GPU.
void BeginFrame(const u8* packet, u64 frame, u32 frames_in_flight);

/// The jitter of the frame's scene draws; zero without an upscaler.
Offset Jitter();

/// A viewport of the scene at extent, y at the bottom and height negative, moved by the frame's
/// jitter when jittered is set.
vk::Viewport SceneViewport(vk::Extent2D extent, bool jittered = true);

/// Outside rendering, after the tonemap: upscales the toned colour, in the shader read layout,
/// into the output image, which compute shaders then read in the general layout. The depth is in
/// the depth read layout and the velocity in the general layout.
void Record(vk::CommandBuffer cmd);

vk::ImageView OutputView();

} // namespace Gr2Port::Upscaler
