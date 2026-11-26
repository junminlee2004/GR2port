// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <vector>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/ui_items.h"
#include "graphics/gr2port/ui_items_internal.h"

namespace Gr2Port::UiItems {

VAddr g_base = 0;

namespace {

// The item pools: the pool the game fills, each pool's memory and the bytes it took.
constexpr VAddr ItemPool = 0x1f21bac;
constexpr VAddr ItemPools = 0x1f21bf0;
constexpr VAddr ItemPoolTaken = 0x1f21c08;
constexpr u32 ItemPoolCount = 3;
// An item's function: + 0x10 of its virtual table.
constexpr u32 ItemFunction = 0x10;
using ItemFunctionPointer = void PS4_SYSV_ABI (*)(VAddr item);

// The UI target the walk begins with: the frame's back buffer object, or the whole-screen target
// the game draws into instead while a pool's flag is set.
constexpr VAddr BackBuffer = 0x1f315a8;
constexpr VAddr ScreenTarget = 0x1f26358;
constexpr VAddr ScreenTargetFlags = 0x1f26c90;

// A target object's size.
constexpr u64 TargetWidth = 0x14;
constexpr u64 TargetHeight = 0x16;

// void FUN_0150aba0(float blur, target, s32 mode): ScreenDraw's copy of a target, blurred when
// blur > 0, into the UI target.
constexpr Hooks::Entry BackgroundEntry{0x150aba0, 6, Hooks::FramePrologue};
// FUN_0161b3e0, a target's resolve, which ScreenDraw's colour control calls on its copy between
// its two draws, returning to ColorControlCall.
constexpr Hooks::Entry ResolveEntry{0x161b3e0, 6, Hooks::FramePrologue};
constexpr VAddr ColorControlCall = 0x84d6cb;

// Room for the frame's copies: constants, palettes, and the words of targets and textures.
constexpr u32 ArenaBytes = 2u << 20;
// The words of a target object Write reads, its size at + 0x14 and its T# at + 0x30, and a T#'s
// and an S#'s.
constexpr u32 TargetWords = 0x50;
constexpr u32 SharpBytes = 32;
constexpr u32 SamplerBytes = 16;

std::vector<Draw> g_draws;
std::unique_ptr<u8[]> g_arena;
u32 g_arena_used = 0;
bool g_started = false;
VAddr g_target = 0;
bool g_kept = false;
VAddr g_vtable = 0;
std::vector<Kind> g_kinds;
u32 g_unknown_logged = 0;
std::vector<VAddr> g_unknown;

// The walk's pool: the one whose memory holds item, else the one two behind the game's.
u32 PoolOf(VAddr item) {
    for (u32 pool = 0; pool < ItemPoolCount; pool++) {
        const VAddr memory = Load<VAddr>(Game(ItemPools) + pool * sizeof(VAddr));
        const u32 taken = Load<u32>(Game(ItemPoolTaken) + pool * sizeof(u32));
        if (memory != 0 && item >= memory && item < memory + taken) {
            return pool;
        }
    }
    return (Load<u32>(Game(ItemPool)) + 2) % ItemPoolCount;
}

// The frame's first item or ScreenDraw draw: the walk's pool and its first target.
void Begin(u32 pool) {
    if (g_started) {
        return;
    }
    g_started = true;
    const bool screen = Load<u8>(Game(ScreenTargetFlags) + pool) != 0;
    g_target = screen ? Game(ScreenTarget) : Load<VAddr>(Game(BackBuffer));
}

void PS4_SYSV_ABI OnBackground(u32, const Hooks::Registers* registers, u64) {
    Begin((Load<u32>(Game(ItemPool)) + 2) % ItemPoolCount);
    float blur;
    std::memcpy(&blur, &registers->xmm0, sizeof(blur));
    g_kept = false;
    g_vtable = 0;
    Background(blur, registers->rdi, static_cast<u32>(registers->rsi));
}

// Write reads the targets' size and T# and the textures' T# and S# when the frame ends, by when an
// item's objects can be gone (FUN_0084b700 frees deferred textures after the walk). The draws from
// first on, an item's, keep those words as its function left them, as the game's binders took
// them when it drew.
void Settle(size_t first) {
    for (size_t i = first; i < g_draws.size(); i++) {
        Draw& draw = g_draws[i];
        if (draw.target != 0) {
            draw.target = Keep(reinterpret_cast<const void*>(draw.target), TargetWords);
        }
        for (Texture& texture : draw.textures) {
            if (texture.sharp != 0) {
                texture.sharp = Keep(reinterpret_cast<const void*>(texture.sharp), SharpBytes);
            }
            if (texture.sampler != 0) {
                texture.sampler =
                    Keep(reinterpret_cast<const void*>(texture.sampler), SamplerBytes);
            }
        }
    }
}

// The colour control's draws, as the game draws them: after the walk, while the frame's back
// buffer is the one it draws into.
void PS4_SYSV_ABI OnResolve(u32, const Hooks::Registers* registers, u64) {
    if (registers->ret != Game(ColorControlCall)) {
        return;
    }
    Begin((Load<u32>(Game(ItemPool)) + 2) % ItemPoolCount);
    g_kept = false;
    g_vtable = 0;
    ColorControl();
}

} // namespace

void Add(Draw draw) {
    draw.kept = g_kept;
    draw.item = g_vtable;
    g_draws.push_back(draw);
}

VAddr Keep(const void* data, u32 bytes) {
    constexpr u32 Alignment = 16;
    if (bytes == 0 || g_arena_used + bytes > ArenaBytes) {
        return 0;
    }
    u8* const at = g_arena.get() + g_arena_used;
    std::memcpy(at, data, bytes);
    g_arena_used = (g_arena_used + bytes + Alignment - 1) & ~(Alignment - 1);
    return reinterpret_cast<VAddr>(at);
}

VAddr Target() {
    return g_target;
}

void SetTarget(VAddr target) {
    g_target = target;
}

std::array<float, 4> Viewport(VAddr target) {
    const float width = static_cast<float>(Load<u16>(target + TargetWidth));
    const float height = static_cast<float>(Load<u16>(target + TargetHeight));
    return {0.5f * width, 0.5f * width, -0.5f * height, 0.5f * height};
}

u32 Blend(s32 mode) {
    // The factors (colour source and destination, alpha source and destination) FUN_01509d90
    // sets, added, with separate alpha.
    struct Factors {
        u32 color_src;
        u32 color_dst;
        u32 alpha_src;
        u32 alpha_dst;
    };
    constexpr std::array<Factors, 6> Modes{{
        {1, 5, 0, 5},
        {1, 1, 0, 1},
        {8, 5, 0, 1},
        {9, 5, 0, 1},
        {9, 1, 0, 1},
        {9, 3, 0, 1},
    }};
    if (mode < 1 || mode > static_cast<s32>(Modes.size())) {
        return 0;
    }
    const Factors& f = Modes[mode - 1];
    return 1u << 30 | 1u << 29 | f.alpha_dst << 24 | f.alpha_src << 16 | f.color_dst << 8 |
           f.color_src;
}

void Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    g_arena = std::make_unique<u8[]>(ArenaBytes);
    g_draws.reserve(Scene::MaxUiDraws);
    for (const auto kinds : {ElementKinds(), TextKinds(), PassKinds()}) {
        g_kinds.insert(g_kinds.end(), kinds.begin(), kinds.end());
    }
    std::ranges::sort(g_kinds, {}, &Kind::vtable);
    if (!Hooks::Hooked(
            Hooks::ObserveEntry(module_base, BackgroundEntry, c, capacity, &OnBackground, 0))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the screen drawing's copy is not the expected code");
    }
    if (!Hooks::Hooked(
            Hooks::ObserveEntry(module_base, ResolveEntry, c, capacity, &OnResolve, 0))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the targets' resolve is not the expected code");
    }
}

void RunItem(VAddr item, bool kept) {
    Begin(PoolOf(item));
    const VAddr vtable = Load<VAddr>(item);
    g_kept = kept;
    g_vtable = vtable;
    const size_t first = g_draws.size();
    const auto kind = std::ranges::lower_bound(g_kinds, vtable, {}, &Kind::vtable);
    const bool known = kind != g_kinds.end() && kind->vtable == vtable;
    if (known) {
        kind->read(item);
    } else if (g_unknown_logged < 64 && std::ranges::find(g_unknown, vtable) == g_unknown.end()) {
        g_unknown.push_back(vtable);
        g_unknown_logged++;
        LOG_INFO(Render_Vulkan, "GR2port UI: draw item of virtual table {:#x} not read",
                 vtable - g_base + Hooks::CodeBase);
    }
    Core::Recompiled::Call(Load<ItemFunctionPointer>(vtable + ItemFunction), item);
    if (known && kind->after != nullptr) {
        kind->after(item);
    }
    Settle(first);
    g_kept = false;
    g_vtable = 0;
}

std::span<const Draw> Draws() {
    return g_draws;
}

void Clear() {
    g_draws.clear();
    g_arena_used = 0;
    g_started = false;
}

} // namespace Gr2Port::UiItems
