// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstddef>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/light_owner.h"

namespace Gr2Port::LightOwner {

namespace {

// FUN_00768940, in the loop over an effect's lights: mov rax, [rax + 0x10]; mov cl, [rax + 0x71]
// reads the effect's object, with the effect at [rsp + 0x70].
constexpr Hooks::Entry Object{0x768d54, 7, {0x48, 0x8b, 0x40, 0x10, 0x8a, 0x48, 0x71}};
constexpr VAddr ObjectNext = 0x768d5b;
constexpr s32 EffectSlot = 0x70;
// The loop's next light. The light count rises only after a light's record is whole, so the next
// light writes over the skipped one's part of a record.
constexpr VAddr NextLight = 0x768c86;

// The stub and what it keeps take 132 bytes, after up to 15 bytes of alignment.
constexpr size_t StubBytes = 160;

// What the stub keeps of the skipped lights: their number, and the last one's effect with the
// effect's first words at that moment.
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
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the effect lights' guard");
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

    const void* object = c.getCurr();
    Xbyak::Label missing;
    c.mov(rax, qword[rax + 0x10]);
    c.test(rax, rax);
    c.jz(missing);
    c.mov(cl, byte[rax + 0x71]);
    c.jmp(Hooks::Code(module_base, ObjectNext));
    // rax and rcx are free at the next light, which sets both before it reads them.
    c.L(missing);
    c.lock();
    c.inc(dword[at(offsetof(Skips, count))]);
    c.mov(rcx, qword[rsp + EffectSlot]);
    c.mov(qword[at(offsetof(Skips, effect))], rcx);
    for (u32 i = 0; i < 4; i++) {
        c.mov(rax, qword[rcx + static_cast<int>(i * 8)]);
        c.mov(qword[at(offsetof(Skips, words) + i * 8)], rax);
    }
    c.jmp(Hooks::Code(module_base, NextLight));

    if (!Hooks::Holds(module_base, Object) ||
        Hooks::Replace(module_base, Object, c, capacity, object) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the effect lights' gathering is not the expected code");
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
                "GR2port: the game's effect lights skipped {} lights since the start, their "
                "effect's object at +0x10 missing; the last effect {:#x} held {:#x} {:#x} {:#x} "
                "{:#x}",
                count, g_skips->effect, g_skips->words[0], g_skips->words[1], g_skips->words[2],
                g_skips->words[3]);
}

} // namespace Gr2Port::LightOwner
