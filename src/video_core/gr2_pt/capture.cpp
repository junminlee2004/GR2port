// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "video_core/gr2_pt/capture.h"
#include "video_core/gr2_pt/scene.h"
#include "video_core/gr2_pt/scene_ring.h"

namespace Gr2Pt::Capture {

namespace {

std::atomic<u64> g_frames{0};

} // namespace

void OnFrame() {
    const u64 frame = g_frames.load(std::memory_order_relaxed) + 1;
    g_frames.store(frame, std::memory_order_relaxed);
    u8* const packet = SceneRing::Begin();
    if (packet == nullptr) {
        return;
    }
    auto* header = reinterpret_cast<Scene::Header*>(packet);
    header->frame = frame;
    SceneRing::Publish(frame);
}

u64 Frames() {
    return g_frames.load(std::memory_order_relaxed);
}

} // namespace Gr2Pt::Capture
