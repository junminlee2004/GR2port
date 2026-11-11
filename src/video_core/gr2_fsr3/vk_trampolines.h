// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Gr2Fsr3 {

/// Sets the dispatcher of the Vulkan commands that the FidelityFX libraries and the FSR 4 v07
/// provider call: GR2port's device's. Call before the first FidelityFX call; it must outlive them.
void SetDispatcher(const vk::detail::DispatchLoaderDynamic& dispatcher);

} // namespace Gr2Fsr3
