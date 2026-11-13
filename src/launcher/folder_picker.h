// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <windows.h>

namespace Gr2Port::Launcher {

/// The folder the player picks in Windows' folder picker over owner, or nothing.
std::optional<std::filesystem::path> PickFolder(HWND owner, const std::wstring& title);

} // namespace Gr2Port::Launcher
