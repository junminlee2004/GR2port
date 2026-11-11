// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace Frontend {
class WindowSDL;
}

namespace Gr2Port {

/// A flip's place in the video out's queue. It holds nothing: only the renderer presents.
struct Frame {};

/// The emulator's side of the game's flips: runs ImGui's frame at each one, whose windows the
/// renderer draws over its picture. A flip waits for one of a few frames, so the game cannot
/// queue flips faster than the present thread takes them.
class Presenter {
public:
    explicit Presenter(Frontend::WindowSDL& window);
    ~Presenter();

    /// The video out reports a display without HDR.
    bool IsHDRSupported() const {
        return false;
    }
    void SetHDR(bool) {}

    /// The flipping thread or the present thread: a free frame, once there is one.
    Frame* PrepareBlankFrame(bool present_thread);
    /// Present thread: the frame of the last flip, or null before the first.
    Frame* PrepareLastFrame();
    /// Present thread: frees frame, unless it is the last flip's frame shown again.
    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true);

private:
    std::array<Frame, 3> frames;
    std::queue<Frame*> free_queue;
    Frame* last_frame = nullptr;
    std::mutex free_mutex;
    std::condition_variable free_cv;
};

} // namespace Gr2Port
