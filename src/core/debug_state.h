// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <queue>

#include "common/types.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <Windows.h>
using ThreadID = DWORD;
#else
#include <pthread.h>
#include <signal.h>
using ThreadID = pthread_t;
#endif

namespace Core::Devtools {
class Layer;
namespace Widget {
class FrameGraph;
} // namespace Widget
} // namespace Core::Devtools

namespace DebugStateType {

extern bool showing_debug_menu_bar;

class DebugStateImpl {
    friend class Core::Devtools::Layer;
    friend class Core::Devtools::Widget::FrameGraph;

    std::queue<std::string> debug_message_popup;

    std::mutex guest_threads_mutex{};
    std::vector<ThreadID> guest_threads{};
    std::atomic_bool is_guest_threads_paused = false;
    u64 pause_time{};

    std::atomic_int32_t flip_frame_count = 0;
    std::atomic_int32_t gnm_frame_count = 0;
    // Counted during the frame, then folded into a running average when the frame flips. The raw
    // per frame count changes too fast to read while playing, so only the average is shown.
    std::atomic_int32_t draw_call_count = 0;
    std::atomic<float> draw_call_count_avg = 0.0f;
    std::atomic_int32_t dispatch_count = 0;
    std::atomic<float> dispatch_count_avg = 0.0f;

public:
    float Framerate = 1.0f / 60.0f;
    float FrameDeltaTime;

    std::pair<u32, u32> game_resolution{};
    std::pair<u32, u32> output_resolution{};
    bool is_using_fsr{};

    void ShowDebugMessage(std::string message) {
        if (message.empty()) {
            return;
        }
        debug_message_popup.push(std::move(message));
    }

    bool& IsShowingDebugMenuBar() {
        return showing_debug_menu_bar;
    }

    void AddCurrentThreadToGuestList();

    void RemoveCurrentThreadFromGuestList();

    void PauseGuestThreads();

    void ResumeGuestThreads();

    bool IsGuestThreadsPaused() const {
        return is_guest_threads_paused;
    }

    void IncFlipFrameNum() {
        // Low weight on the newest frame, so the average settles instead of jumping around.
        static constexpr float smoothing = 0.05f;
        const auto draws = draw_call_count.exchange(0, std::memory_order_relaxed);
        const auto draws_avg = draw_call_count_avg.load(std::memory_order_relaxed);
        draw_call_count_avg.store(draws_avg + (float(draws) - draws_avg) * smoothing,
                                  std::memory_order_relaxed);

        const auto dispatches = dispatch_count.exchange(0, std::memory_order_relaxed);
        const auto dispatches_avg = dispatch_count_avg.load(std::memory_order_relaxed);
        dispatch_count_avg.store(dispatches_avg + (float(dispatches) - dispatches_avg) * smoothing,
                                 std::memory_order_relaxed);
        ++flip_frame_count;
    }

    void IncDrawCall() noexcept {
        // Single writer (the GPU command thread): a plain load/store pair
        // avoids the locked RMW on the draw path. Readers are stats-only and
        // may miss a transient count.
        draw_call_count.store(draw_call_count.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
    }

    void IncDispatch() noexcept {
        dispatch_count.store(dispatch_count.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
    }

    [[nodiscard]] float GetDrawCallsAvg() const noexcept {
        return draw_call_count_avg.load(std::memory_order_relaxed);
    }

    [[nodiscard]] float GetDispatchesAvg() const noexcept {
        return dispatch_count_avg.load(std::memory_order_relaxed);
    }

    void IncGnmFrameNum() {
        ++gnm_frame_count;
    }

    u32 GetFrameNum() const {
        return flip_frame_count;
    }
};
} // namespace DebugStateType

extern DebugStateType::DebugStateImpl& DebugState;
