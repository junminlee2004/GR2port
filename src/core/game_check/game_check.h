// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// GR2port starts only Gravity Rush 2 v1.11, from the executable's own folder: the game and its
// update as CUSAxxxxx and CUSAxxxxx-patch, or one with the update copied over it, each a folder or
// a .zar archive. The checks run at every start.
namespace Gr2Port::GameCheck {

struct Game {
    /// The game's folder or .zar archive; the emulator finds the update beside it.
    std::filesystem::path root;
    /// The four .psarc archives are extracted to folders of their names.
    bool loose_files = false;
};

/// Finds the game and checks the dump. On a problem, tells the player in a message box and
/// returns nothing.
std::optional<Game> Find();

/// Whether Find found the four archives extracted to folders.
bool LooseFiles();

/// The title ID of the game Find found, such as CUSA04943.
std::string TitleId();

/// The Gravity Rush 2 patch XML built into the executable.
std::string_view PatchXml();

} // namespace Gr2Port::GameCheck
