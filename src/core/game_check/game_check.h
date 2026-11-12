// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

// GR2port starts only Gravity Rush 2 v1.11, from the executable's own folder: the game and its
// update, or one with the update copied over it, each a folder or a .zar archive of any name. The
// param.sfo of each tells what it holds. The checks run at every start.
namespace Gr2Port::GameCheck {

struct Game {
    /// The game's folder or .zar archive. The mounts take the update's from the check.
    std::filesystem::path root;
    /// The four .psarc archives are extracted, to folders of their names or over the dump.
    bool loose_files = false;
};

/// Finds the game and checks the dump. On a problem, tells the player in a message box and
/// returns nothing.
std::optional<Game> Find();

/// Whether Find found the four archives extracted to folders.
bool LooseFiles();

/// The title ID of the game Find found, such as CUSA04943.
std::string TitleId();

/// The copies of a file of the game Find found, the update's before the game's.
std::vector<std::vector<u8>> Files(std::string_view path);

/// The Gravity Rush 2 patch XML built into the executable.
std::string_view PatchXml();

} // namespace Gr2Port::GameCheck
