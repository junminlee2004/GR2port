// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include "common/types.h"

// The launcher's updater. When the launcher opens it asks GitHub for GR2port's latest stable
// release, and when the player agrees it puts that release's files in place of this build's.
namespace Gr2Port::Launcher::Updater {

/// A stable release whose build is ahead of this one.
struct Release {
    /// The release's title and its notes, as written on GitHub.
    std::string name;
    std::string notes;
    /// The address of the zip for this system.
    std::string zip;
};

/// A download as its window shows it.
struct Download {
    std::atomic<u64> received{};
    std::atomic<u64> size{};
    /// The window sets it to end the download.
    std::atomic<bool> stopped{};
};

/// Asks GitHub for the latest stable release on a thread of its own; pre-releases do not count.
/// The answer holds the release when its build is ahead of this one, and nothing otherwise or
/// when GitHub does not answer.
std::future<std::optional<Release>> Check();

/// Downloads release's zip on a thread of its own and puts its files in place: the executable
/// over this one, userdata's files in the user folder and the others beside the executable. The
/// answer tells whether they are in place, for the launcher to restart.
std::future<bool> Install(Release release, std::shared_ptr<Download> download);

} // namespace Gr2Port::Launcher::Updater
