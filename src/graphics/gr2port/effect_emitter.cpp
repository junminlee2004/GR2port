// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstddef>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/effect_emitter.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::EffectEmitter {

namespace {

// FUN_00768940, in the loop over an effect's draw nodes: mov rax, [rax + 0x10] and
// mov cl, [rax + 0x71] read the effect's emitter, with the effect at [rsp + 0x70].
constexpr Hooks::Entry Emitter{0x768d54, 7, {0x48, 0x8b, 0x40, 0x10, 0x8a, 0x48, 0x71}};
constexpr VAddr EmitterNext = 0x768d5b;
constexpr s32 EffectSlot = 0x70;
// The loop's next draw node. The item count rises only after an item is whole, so the next item
// writes over the skipped one's part of an item.
constexpr VAddr NextNode = 0x768c86;

// The stub and what it keeps take 132 bytes, after up to 15 bytes of alignment.
constexpr size_t StubBytes = 160;

// What the stub keeps of the skipped items: their number, and the last one's effect with the
// effect's first words at that moment. The upper half of the second word is the effect's state,
// which the main thread sets to 2 when it ends the effect.
struct Skips {
    u32 count;
    u32 reported;
    u64 effect;
    u64 words[4];
};
Skips* g_skips = nullptr;

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the effect emitters' guard");
        return false;
    }
    c.align(16);
    Xbyak::Label skips;
    c.L(skips);
    g_skips = reinterpret_cast<Skips*>(const_cast<u8*>(c.getCurr()));
    for (size_t i = 0; i < sizeof(Skips) / 4; i++) {
        c.dd(0);
    }
    const auto at = [&skips](size_t offset) { return rip + skips + static_cast<int>(offset); };

    const void* emitter = c.getCurr();
    Xbyak::Label missing;
    c.mov(rax, qword[rax + 0x10]);
    c.test(rax, rax);
    c.jz(missing);
    c.mov(cl, byte[rax + 0x71]);
    c.jmp(Hooks::Code(module_base, EmitterNext));
    // rax and rcx are free at the next draw node, which sets both before it reads them.
    c.L(missing);
    c.lock();
    c.inc(dword[at(offsetof(Skips, count))]);
    c.mov(rcx, qword[rsp + EffectSlot]);
    c.mov(qword[at(offsetof(Skips, effect))], rcx);
    for (u32 i = 0; i < 4; i++) {
        c.mov(rax, qword[rcx + static_cast<int>(i * 8)]);
        c.mov(qword[at(offsetof(Skips, words) + i * 8)], rax);
    }
    c.jmp(Hooks::Code(module_base, NextNode));

    if (!Hooks::Holds(module_base, Emitter) ||
        Hooks::Replace(module_base, Emitter, c, capacity, emitter) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the effects' draw items are not the expected code");
        g_skips = nullptr;
        return false;
    }
    return true;
}

void Report() {
    if (g_skips == nullptr) {
        return;
    }
    const u32 count = std::atomic_ref<u32>{g_skips->count}.load(std::memory_order_relaxed);
    if (count == g_skips->reported) {
        return;
    }
    g_skips->reported = count;
    LOG_WARNING(Render_Vulkan,
                "GR2port: the game's effects skipped {} draw items since the start, their "
                "effect's emitter at +0x10 gone; the last effect {:#x}, in state {}, held {:#x} "
                "{:#x} {:#x} {:#x}",
                count, g_skips->effect, g_skips->words[1] >> 32, g_skips->words[0],
                g_skips->words[1], g_skips->words[2], g_skips->words[3]);
}

} // namespace Gr2Port::EffectEmitter
