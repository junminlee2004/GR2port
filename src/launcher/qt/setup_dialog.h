// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QWidget;

namespace Gr2Port::Launcher {

class Art;

/// Shows the Setup dialog over owner until Ok or Cancel. Ok saves the settings. Returns whether
/// it saved a change of the Paths tab, which the executable takes when it starts again.
bool ShowSetup(QWidget* owner, const Art& art);

} // namespace Gr2Port::Launcher
