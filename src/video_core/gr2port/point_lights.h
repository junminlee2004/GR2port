// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/gr2port/device.h"
#include "video_core/gr2port/scene.h"

namespace Gr2Port {
class Memory;
} // namespace Gr2Port

// The view's point lights, on the render thread: the game's LightCompute lists the lights that
// reach into each cell of a grid over the view, and its lit programs read a pixel's cell.
namespace Gr2Port::PointLights {

/// Creates the grid and the program that fills it.
bool Start(const Device& device, Memory& memory);

/// Outside rendering, before the passes that light with the point lights: fills the grid from the
/// packet's lights, when it has any.
void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address);

/// The grid's device address: Scene::LightsPerCell light indices to a cell.
u64 GridAddress();

} // namespace Gr2Port::PointLights
