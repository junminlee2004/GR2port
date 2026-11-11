// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "common/types.h"

// The game's trophies as the launcher's trophy viewer lists them, from the trophy files in the
// user folder.
namespace Gr2Port::Trophies {

struct Trophy {
    s32 id = 0;
    /// 'B' for bronze, 'S' for silver, 'G' for gold or 'P' for platinum.
    char grade = 'B';
    bool hidden = false;
    bool earned = false;
    /// When the player earned it, in seconds since 1970; 0 when the files do not say.
    u64 earned_at = 0;
    std::string name;
    std::string detail;
    std::filesystem::path icon;
};

struct List {
    std::string title;
    std::vector<Trophy> trophies;
};

/// Extracts the trophy files of the game GameCheck found to the user folder, unless they are
/// there. The game does the same when it starts.
void Extract();

/// The trophy lists in the user folder, with the names and descriptions of a console language
/// where the game has them.
std::vector<List> Read(s32 console_language);

} // namespace Gr2Port::Trophies
