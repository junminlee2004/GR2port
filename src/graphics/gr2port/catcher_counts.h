// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "graphics/gr2port/device.h"
#include "graphics/gr2port/pipeline.h"

namespace Gr2Port {
class Memory;
}

// The GPU Crash Catcher's counts; with the catcher off nothing here runs. The catcher's builds of
// the programs whose loops have limits count each time a loop stops at its limit, and the log says
// when the counts move.
namespace Gr2Port::Catcher::Counts {

/// A catcher build's push constants end with the address of the counts at this offset, which
/// shaders/catcher.glsl names too.
inline constexpr u32 Offset = 120;

/// The push constant bytes of a pipeline made from a program's catcher build when the catcher is
/// on, bytes otherwise.
u32 PushBytes(u32 bytes);

/// With the device made, catcher on: the device's pipeline binds go through the counts.
void Install(Device& device);

/// Render thread, once, catcher on: makes the counts' buffer.
void Start(Memory& memory);

/// A pipeline made from a catcher build: each bind of it sets its counts' address.
void Counting(const Pipeline& pipeline, vk::ShaderStageFlags stages);

/// Render thread: logs the counts when they moved, at most every two seconds.
void Log();

/// Render thread, in a lost device's report: the counts.
void Report();

} // namespace Gr2Port::Catcher::Counts
