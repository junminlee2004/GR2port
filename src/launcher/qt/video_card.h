// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"

namespace Gr2Port::Launcher {

struct VideoCard {
    std::string name;
    // Its own memory and the RAM the system lends it.
    u64 bytes = 0;
    u64 shared = 0;
    // An integrated GPU, whose memory is all RAM.
    bool integrated = false;
};

/// The GPU GR2port draws with, as it picks one: the first that is not integrated, else the first;
/// none when Vulkan lists no hardware GPU. SDL's video subsystem must be started.
VideoCard FindVideoCard();

/// The RAM the system manages, or 0 when it does not tell.
u64 InstalledRam();

} // namespace Gr2Port::Launcher
