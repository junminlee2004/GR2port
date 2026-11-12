// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// The cameras' obstacle test at any frame rate. FUN_0155e8c0 keeps a camera's eye on an obstacle
// while the obstacle lies at most 0.1 m past the distance the camera had the frame before. In a
// frame of 1/30 s a player who moves away faster than 3 m/s gets past that, and the camera follows
// again. In a shorter frame nobody moves 0.1 m, so the eye stays on the obstacle and the distance
// grows with the player. Here the 0.1 m shrinks with the frame time, so the camera lets go at
// 3 m/s at every frame rate.
namespace Gr2Port::CameraReach {

/// Hooks the obstacle test's comparison of the hit's distance.
bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity);

} // namespace Gr2Port::CameraReach
