// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The window the executable opens with once the game checks pass: the game's splash with Play,
// Options and Quit, in the style of Nixxes' launcher for Rise of the Tomb Raider. -nolauncher
// skips it.
namespace Gr2Port::Launcher {

/// Sends standard output and error to the terminal that started the executable, which as a
/// Windows program has no console of its own. Redirected output stays as it is.
void AttachTerminal();

/// Shows the launcher until the player picks Play or Quit, and returns whether it was Play.
bool Run();

} // namespace Gr2Port::Launcher
