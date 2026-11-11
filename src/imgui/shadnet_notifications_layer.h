// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// GR2port shows none of shadNet's overlays: a notification the handlers push is dropped.
namespace ImGui::ShadNetNotify {

enum class Kind {
    FriendRequest,
    FriendNew,
    FriendLost,
    Online,
    Info,
    Warning,
};

inline void Push(Kind, std::string) {}

} // namespace ImGui::ShadNetNotify
