// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/logging/log.h"
#include "graphics/gr2port/emulated_gpu.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::EmulatedGpu {

namespace {

// SubmitCategory FUN_014214e0, the frame graph's per-item draw loop. Its handlers read the items
// and write GNM commands, which nothing reads.
constexpr Hooks::Entry DrawLoop{0x14214e0, 6, Hooks::FramePrologue};

// What the game calls for each draw and dispatch on a graphics context. FUN_0042dc50 binds a
// draw's shaders and writes their constants, FUN_0042da70 does the same for a dispatch, and the
// others write the draw and dispatch packets. Each takes its arguments in registers.
constexpr std::array Writers{
    Hooks::Entry{0x42dc50, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42da70, 6, Hooks::FramePrologue},
    Hooks::Entry{0x412c50, 6, Hooks::FramePrologue},
    Hooks::Entry{0x412820, 6, Hooks::FramePrologue},
    Hooks::Entry{0x4135d0, 6, Hooks::FramePrologue},
};

// The same for a dispatch on a compute queue, FUN_0042cfb0 and FUN_00413560. Nothing reads a
// compute queue's dispatches, so they return at once.
constexpr std::array ComputeWriters{
    Hooks::Entry{0x42cfb0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x413560, 6, Hooks::FramePrologue},
};

using WriterFunction = void PS4_SYSV_ABI (*)(u64, u64, u64, u64, u64, u64);
std::array<WriterFunction, Writers.size()> g_writers{};

// On the game's draw thread: whether the frame graph runs.
thread_local bool t_frame_graph = false;

// In place of writer Index. The frame graph's draws and dispatches are not written.
template <size_t Index>
void PS4_SYSV_ABI OnWriter(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    if (!t_frame_graph) {
        g_writers[Index](a, b, c, d, e, f);
    }
}

} // namespace

void Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    if (Hooks::SetReturning(module_base, DrawLoop, true) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the draw loop is not the expected code");
    }
    const std::array<const void*, Writers.size()> targets{
        reinterpret_cast<const void*>(&OnWriter<0>), reinterpret_cast<const void*>(&OnWriter<1>),
        reinterpret_cast<const void*>(&OnWriter<2>), reinterpret_cast<const void*>(&OnWriter<3>),
        reinterpret_cast<const void*>(&OnWriter<4>),
    };
    for (size_t i = 0; i < Writers.size(); i++) {
        const void* original = nullptr;
        if (Hooks::Replace(module_base, Writers[i], c, capacity, targets[i], &original) !=
            Hooks::Result::Hooked) {
            LOG_ERROR(Render_Vulkan, "GR2port: the function at {:#x} is not the expected code",
                      Writers[i].address);
            continue;
        }
        g_writers[i] = reinterpret_cast<WriterFunction>(const_cast<void*>(original));
    }
    for (const Hooks::Entry& entry : ComputeWriters) {
        if (Hooks::Return(module_base, entry) != Hooks::Result::Hooked) {
            LOG_ERROR(Render_Vulkan, "GR2port: the function at {:#x} is not the expected code",
                      entry.address);
        }
    }
}

void SetFrameGraphRunning(bool running) {
    t_frame_graph = running;
}

bool On() {
    return false;
}

} // namespace Gr2Port::EmulatedGpu
