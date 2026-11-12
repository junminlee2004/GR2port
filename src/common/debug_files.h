// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

namespace Common {

// The log folder holds log.txt. A start with --debug-files adds the developers' files to it: the
// pictures of a frame and of a photo, every frame's pass times, the module dump and ImGui's log.
inline std::atomic_bool g_debug_files{false};

inline bool DebugFiles() {
    return g_debug_files.load(std::memory_order_relaxed);
}

inline void SetDebugFiles(bool wanted) {
    g_debug_files.store(wanted, std::memory_order_relaxed);
}

} // namespace Common
