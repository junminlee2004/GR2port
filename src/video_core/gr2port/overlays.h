// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Gr2Port {

/// At the start of main, before any window or Vulkan instance: keeps overlays that hook the
/// process out of it while it holds two Vulkan devices.
void KeepOverlaysOut();

/// Logs whether an overlay's hooks are in the process.
void LogOverlays();

} // namespace Gr2Port
