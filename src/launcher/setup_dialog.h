// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <windows.h>

namespace Gr2Port::Launcher {

class Art;

/// Shows the Setup dialog over owner until Ok or Cancel. Ok saves the display settings.
void ShowSetup(HWND owner, const Art& art);

} // namespace Gr2Port::Launcher
