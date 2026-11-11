// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>
#include <queue>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Frontend {
class WindowSDL;
}

namespace Vulkan {

/// GR2port: a flip's place in the present queue. It has no image: only the renderer's window
/// presents.
struct Frame {
    vk::Fence present_done;
    vk::Semaphore ready_semaphore;
    u64 ready_tick;
    u8 id{};
};

/// GR2port: paces the game's flips and runs ImGui, whose windows GR2port's renderer draws.
class Presenter {
public:
    explicit Presenter(Frontend::WindowSDL& window);
    ~Presenter();

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    bool IsHDRSupported() const {
        return swapchain.HasHDR();
    }

    void SetHDR(bool enable) {
        if (!IsHDRSupported()) {
            return;
        }
        swapchain.SetHDR(enable);
    }

    /// If present_thread is true, the present thread's scheduler submits for the frame.
    Frame* PrepareBlankFrame(bool present_thread);

    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);
    Frame* PrepareLastFrame();

private:
    Frame* GetRenderFrame();

private:
    Frontend::WindowSDL& window;
    Instance instance;
    Scheduler draw_scheduler;
    Scheduler present_scheduler;
    Scheduler flip_scheduler;
    Swapchain swapchain;
    std::vector<Frame> present_frames;
    std::queue<Frame*> free_queue;
    Frame* last_submit_frame;
    std::mutex free_mutex;
    std::condition_variable free_cv;
};

} // namespace Vulkan
