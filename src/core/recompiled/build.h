// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Recompiled mode's libraries, made on the player's machine from their own game by the generator
// the app carries (build-library.py with its sources, in the app's resources), in the folder
// recompiled beside the executable: once, again for another generator, and a library of the hook
// code for each hook code that GR2port's settings give. A start that loads hook code no library
// holds saves it and starts again, and that start builds its library and goes on to the game.
namespace Core::Recompiled::Build {

/// A build: what comes before the generator's run, the run, and what comes after.
struct Job {
    /// What the progress window says the build is.
    std::string title;
    /// Before the run, off the window's thread. False stops the job.
    std::function<bool()> prepare;
    /// The generator's run: the program, its arguments and the variables added to the
    /// environment. It prints each step on a line of its own.
    std::filesystem::path program;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    /// The file that receives the run's output.
    std::filesystem::path log;
    /// After the run succeeds: puts its libraries in place. False when that fails.
    std::function<bool()> install;
};

/// The folder beside the executable that holds the libraries.
std::filesystem::path Folder();

/// The build this start needs before the game runs, or nothing: the libraries are ready, the
/// setting recompiled_library names libraries of its own, or the app carries no generator. Runs
/// after the game check, which finds the game the build reads.
std::optional<Job> Needed();

/// For a start whose hook code no library holds: where the hook code is to be saved (a prefix
/// for its two files), or nothing when this start goes on without (it cannot build, or it was
/// started for the hook code already).
std::optional<std::filesystem::path> HookDump();

/// After the hook code is saved at HookDump: starts the executable again, which builds the
/// library and goes on to the game. Returns only when that fails.
void RestartForHookCode();

/// At the start: whether RestartForHookCode started this run. Such a run skips the launcher.
bool RestartedForHookCode();

} // namespace Core::Recompiled::Build
