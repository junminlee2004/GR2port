// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"
#include "graphics/gr2port/device.h"

namespace Gr2Port {
class Memory;
}

// The GPU Crash Catcher's marks; with the catcher off nothing here runs. On a device with
// VK_AMD_buffer_marker the GPU writes a number as it reaches each draw and dispatch the render
// thread records and another as it finishes it, so a stall's lines and a lost device's report name
// the calls the GPU did not finish.
namespace Gr2Port::Catcher::Marks {

/// With the device made, catcher on: the device's pipeline calls, and with buffer markers its
/// draws and dispatches, go through the marks from here on.
void Install(Device& device);

/// Render thread, once, catcher on: makes the marks' buffer. The render thread's draws and
/// dispatches are marked from here on.
void Start(Memory& memory);

/// Any thread, while the GPU stalls: the last calls the GPU finished and reached, in a line.
std::string DescribeProgress();

/// Render thread, in a lost device's report: the calls the GPU did not finish and the last one it
/// finished.
void Report();

} // namespace Gr2Port::Catcher::Marks
