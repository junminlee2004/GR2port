// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"

// What the driver can say about a lost device: the pass each pipeline stage last reached, and the
// fault's kind and addresses. A GPU without the extensions marks nothing and reports less.
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

/// Inside or outside a rendering: records that the GPU reached the start of pass.
void Mark(const Device& device, vk::CommandBuffer cmd, Pass pass);

/// Given a call's result: when it says the device is lost, logs the report, once.
void Check(const Device& device, vk::Result result);

} // namespace Gr2Port::DeviceLost
