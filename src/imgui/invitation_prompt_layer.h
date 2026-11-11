// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"

// GR2port shows none of shadNet's overlays: an invitation the handlers push is dropped.
namespace ImGui::InvitationPrompt {

inline void Push(s32, std::string, std::string, std::string) {}
inline void Dismiss(const std::string&) {}

} // namespace ImGui::InvitationPrompt
