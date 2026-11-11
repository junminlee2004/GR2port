// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>

namespace Gr2Port {

/// Around the emulator's own Vulkan instance: overlay layers stay out of it, so they see only the
/// renderer's device. Overlays expect one presenting device per process, and this one holds two.
class OverlaysOutOfInstance {
public:
    OverlaysOutOfInstance();
    ~OverlaysOutOfInstance();
    OverlaysOutOfInstance(const OverlaysOutOfInstance&) = delete;
    OverlaysOutOfInstance& operator=(const OverlaysOutOfInstance&) = delete;

private:
    std::array<std::optional<std::string>, 2> saved;
};

/// Whether an overlay that hooks every device's present is in the process. The emulator's window
/// then presents nothing, so the overlay draws only on the renderer's.
bool OverlayHooksEveryDevice();

/// Logs whether an overlay's hooks are in the process.
void LogOverlays();

} // namespace Gr2Port
