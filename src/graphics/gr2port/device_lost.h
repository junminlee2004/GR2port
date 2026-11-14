// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <source_location>
#include <string_view>

#include "graphics/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

namespace Gr2Port::Scene {
struct Header;
}

// The log of a lost device: the call that reported it, the frames in flight, the memory, the
// driver's checkpoints and fault, the buffer each fault address lies in, and every device address
// of the frames in flight checked against the buffers. With the trace on, the GPU also writes
// each pass it begins into memory the CPU reads, on every GPU.
namespace Gr2Port::DeviceLost {

enum class Pass : u32 {
    FrameStart,
    Deformers,
    Kernels,
    Simulation,
    PointLights,
    MeshSampling,
    SelfTranslucent,
    Shadows,
    FogVolumes,
    Surfaces,
    Lines,
    CharacterDepth,
    Occlusion,
    Velocity,
    Light,
    Characters,
    Sky,
    Contours,
    EffectsBehind,
    Glass,
    Water,
    EffectsInFront,
    FogApply,
    Particles,
    EffectsTopMost,
    Meter,
    Bloom,
    Antialias,
    Composite,
    Ui,
    Present,
    Count,
};

/// Before the renderer starts: turns the trace on for this run. The log names reason.
void Arm(std::string_view reason);

/// Whether the log at path, the last run's, reports a lost device.
bool LastRunLost(const std::filesystem::path& path);

/// Render thread, once: keeps memory for the report and, with the trace on, makes the buffer the
/// GPU writes its passes into. frames_in_flight frames keep their packets until they are done.
void Start(const Device& device, Memory& memory, u32 frames_in_flight);

/// Any thread: a buffer the GPU reads by address exists from here on, or is freed.
void AddBuffer(u64 address, u64 size, const std::source_location& where);
void RemoveBuffer(u64 address);

/// Outside a rendering: records that the GPU reached the start of pass.
void Mark(const Device& device, vk::CommandBuffer cmd, Pass pass);

/// As Mark, inside a rendering whose attachments load: with the trace on, the rendering ends
/// before the mark and begins again after it.
void Mark(const Device& device, vk::CommandBuffer cmd, Pass pass,
          const vk::RenderingInfo& rendering);

/// Render thread, after the frame's first mark: the frame draws packet, which starts with header.
void Note(const Scene::Header& header, const u8* packet);

/// Render thread: the frame went to the GPU.
void Submitted();

/// Given the result of call: when it says the device is lost, logs the report, once.
void Check(const Device& device, vk::Result result, std::string_view call);

} // namespace Gr2Port::DeviceLost
