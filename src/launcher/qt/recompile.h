// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Recompiled::Build {
struct Job;
}

namespace Gr2Port::Launcher {

/// Recompile's windows, in the launcher's Qt application.
bool RecompileWindow(const Core::Recompiled::Build::Job& job);

} // namespace Gr2Port::Launcher
