// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "graphics/gr2port/device.h"
#include "graphics/gr2port/scene.h"

// GPU time of each pass: a timestamp after each, a line per report and a row per frame in
// gr2port_pass_times.csv in the log folder. Without Start, the other functions do nothing.
namespace Gr2Port::PassTimes {

/// Each pass ends at its mark, in recording order.
enum class Pass : u32 {
    Uploads,
    Deformers,
    Kernels,
    SimCopies,
    SimUpdate,
    SimClear,
    SimMove,
    SimSpawn,
    Simulation,
    PointLights,
    SelfTranslucent,
    ShadowOpaque0,
    ShadowAlpha0,
    ShadowLeaves0,
    ShadowParticles0,
    ShadowOpaque1,
    ShadowAlpha1,
    ShadowLeaves1,
    ShadowParticles1,
    ShadowOpaque2,
    ShadowAlpha2,
    ShadowLeaves2,
    Shadows,
    FogBake,
    FogDensity,
    FogShadow,
    FogVolumes,
    WorldOpaque,
    WorldDecals,
    Metaballs,
    Vegetation,
    ParticleSurfaces,
    SurfacesEnd,
    Lines,
    CharacterDepth,
    Occlusion,
    Velocity,
    Light,
    Lambert,
    EnemiesForward,
    Characters,
    Sky,
    EnemiesAlpha,
    CharacterGlass,
    Contours,
    LowOpaque,
    EffectsBg,
    LowTransparent,
    Glass,
    Water,
    EffectsFg,
    FogApply,
    Particles,
    EffectsTopMost,
    Meter,
    Bloom,
    MotionBlur,
    Antialias,
    Composite,
    Ui,
    Present,
    Count,
};

/// Creates the query pool for slots frame slots and opens the file.
void Start(const Device& device, u32 slots);

/// At the start of a frame's commands in slot: resets its marks and writes the first.
void Begin(vk::CommandBuffer cmd, u32 slot, const Scene::Header& header);

/// After pass, once per frame.
void Mark(vk::CommandBuffer cmd, Pass pass);

/// With slot's fence signalled: adds its frame's times.
void Collect(u32 slot);

/// Logs the means, shortest and longest since the last report.
void Report();

} // namespace Gr2Port::PassTimes
