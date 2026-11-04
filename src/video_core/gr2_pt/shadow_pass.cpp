// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

#include "common/logging/log.h"
#include "video_core/gr2_pt/caster_batch.h"
#include "video_core/gr2_pt/hooks.h"
#include "video_core/gr2_pt/marker.h"
#include "video_core/gr2_pt/shadow_pass.h"

namespace Gr2Pt::ShadowPass {

namespace {

// SubmitCategory FUN_014214e0(view, category): runs a category's begin callback, its item
// callback for each item the view lists, and its end callback.
constexpr Hooks::Entry SubmitCategory{0x14214e0, 6, Hooks::FramePrologue};

// The opaque casters' category; only the ShadowCast passes submit it.
constexpr u32 CasterCategory = 0;
// The cascade the view draws, which selects the caster's view-projection.
constexpr u64 ViewCascade = 0x740;
// The context's count of issued draws, which DrawIndexed FUN_016173f0 increments.
constexpr u64 ContextDraws = 0xcec8;

// The game's draw thread is the only caller.
struct DrawThread {
    u32 frame;
    u32 draws;
    std::chrono::steady_clock::time_point start;
};
DrawThread g_draw_thread;

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

u32 Cascade(VAddr view) {
    return std::min(Load<u32>(view + ViewCascade), Cascades - 1);
}

void PS4_SYSV_ABI OnSubmit(u32, const Hooks::Registers* registers, u64) {
    if (static_cast<u32>(registers->rsi) != CasterCategory) {
        return;
    }
    const u32 cascade = Cascade(registers->rdi);
    if (cascade == 0) {
        ++g_draw_thread.frame;
    }
    CasterBatch::Begin(cascade, g_draw_thread.frame);
    g_draw_thread.draws = Load<u32>(Marker::Context() + ContextDraws);
    Marker::Write({Marker::Kind::PassBegin, FirstPass + cascade, g_draw_thread.frame, 0, 0});
    g_draw_thread.start = std::chrono::steady_clock::now();
}

void PS4_SYSV_ABI OnSubmitReturn(u32, const Hooks::Arguments* arguments, u64) {
    if (static_cast<u32>(arguments->rsi) != CasterCategory) {
        return;
    }
    CasterBatch::End();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - g_draw_thread.start)
                        .count();
    const u32 draws = Load<u32>(Marker::Context() + ContextDraws) - g_draw_thread.draws;
    Marker::Write({Marker::Kind::PassEnd, FirstPass + Cascade(arguments->rdi), g_draw_thread.frame,
                   draws, static_cast<u32>(std::min<s64>(ns, std::numeric_limits<u32>::max()))});
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    const Hooks::Result end =
        Hooks::ObserveReturn(module_base, SubmitCategory, c, capacity, &OnSubmitReturn, 0);
    const Hooks::Result begin =
        end == Hooks::Result::Hooked
            ? Hooks::ObserveEntry(module_base, SubmitCategory, c, capacity, &OnSubmit, 0)
            : end;
    if (end == Hooks::Result::Hooked && begin == Hooks::Result::Chained) {
        return true;
    }
    LOG_ERROR(Render_Vulkan, "GR2 PT: SubmitCategory at {:#x} is not hooked ({}, {})",
              SubmitCategory.address, static_cast<u32>(end), static_cast<u32>(begin));
    return false;
}

} // namespace Gr2Pt::ShadowPass
