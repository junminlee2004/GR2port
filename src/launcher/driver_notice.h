// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <windows.h>

namespace Gr2Port::Launcher {

/// Over owner, tells a player with an NVIDIA RTX GPU, the GPUs that have DLSS, to update a driver
/// older than the one DLSS needs. Its Don't show again box turns the notice off for good.
void ShowDriverNotice(HWND owner);

} // namespace Gr2Port::Launcher
