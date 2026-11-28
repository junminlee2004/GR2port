// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Core::Recompiled::Build {
struct Job;
}

// The window the executable opens with once the game checks pass: the game's splash with Play,
// Options and Quit, in the style of Nixxes' launcher for Rise of the Tomb Raider. -nolauncher
// skips it.
namespace Gr2Port::Launcher {

/// Sends standard output and error to the terminal that started the executable, which as a
/// Windows program has no console of its own. Redirected output stays as it is.
void AttachTerminal();

/// Shows text, the game check's message that no game is found, with an OK button and under it a
/// link to choose the folder that holds the game instead. Returns the folder the player chose in
/// the folder picker the link opens, or nothing.
std::optional<std::filesystem::path> AskGameFolder(const std::string& text);

/// Shows the launcher until the player picks Play or Quit, and returns whether it was Play.
bool Run();

/// Runs a build of recompiled mode's libraries behind a progress window, after Apple's Command
/// Line Tools, which it needs, are there. Returns whether the libraries are in place; false when
/// the player stops it or it fails, which the window tells.
bool Recompile(const Core::Recompiled::Build::Job& job);

} // namespace Gr2Port::Launcher
