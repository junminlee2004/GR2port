// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/low_energy_alert.h"
#include "graphics/gr2port/settings.h"

namespace Gr2Port::LowEnergyAlert {

namespace {

// FUN_00901720, Kat's frame: vmovss xmm0, [1.0] loads the volume that the alert's two sounds,
// energy_empty and energy_empty2, play at.
constexpr Hooks::Entry AlertVolume{0x901d7f, 8, {0xc5, 0xfa, 0x10, 0x05, 0x29, 0x67, 0xfb, 0x00}};

// FUN_014e3370, which starts a sound: mov edi, r15d; call FUN_00672d80 sets the volume of the
// sound it started, whose handle is r15d, when that volume is not 1.0. The function keeps the
// volume at StartedVolume of its frame.
constexpr Hooks::Entry StartVolume{0x14e33bd, 8, {0x44, 0x89, 0xff, 0xe8, 0xbb, 0xf9, 0x18, 0xff}};
constexpr s32 StartedVolume = -0x1c;
// s32 FUN_00672d80(float volume, s32 handle): sets a sound's volume, at most 1.0.
constexpr VAddr SetVolume = 0x672d80;
// s32 FUN_0066e550(s32 handle, u32 command, const float* value, u8 flag, const char* name): a
// command to the sound engine. WideVolume sets the volume that FUN_00672d80's command sets, at
// most 4.0. FUN_00672d80 gives its command the flag 1 and the name at VolumeName.
constexpr VAddr SoundCommand = 0x66e550;
constexpr u32 WideVolume = 0xc;
constexpr VAddr VolumeName = 0x182a37b;

// The slider's position of the game's own volume, and its highest.
constexpr u32 OwnPosition = 50;
constexpr u32 TopPosition = 100;

// The two stubs, their jumps from the sites, and the room the last jump's Replace asks for.
constexpr size_t StubBytes = 128 + 2 * 14 + Hooks::MaxStubBytes;

// What the stubs read and Set writes: the alert's volume, and that volume again when it is above
// 1.0, else 0.
struct Volumes {
    float volume = 1.0f;
    float wide = 0.0f;
};
Volumes g_volumes;
static_assert(std::atomic_ref<float>::is_always_lock_free);

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (!Hooks::Holds(module_base, AlertVolume) || !Hooks::Holds(module_base, StartVolume)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the low energy alert's sound is not the expected code");
        return false;
    }
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the hooks of the low energy alert's sound");
        return false;
    }
    Set(Settings::Get().game.low_energy_alert);
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };

    const void* alert = c.getCurr();
    c.push(rax);
    c.mov(rax, reinterpret_cast<u64>(&g_volumes));
    c.vmovss(xmm0, dword[rax + offsetof(Volumes, volume)]);
    c.pop(rax);
    c.jmp(code(AlertVolume.address + AlertVolume.size));

    // FUN_00672d80 leaves a volume above 1.0 at 1.0, so a sound that starts at the alert's volume
    // above 1.0 then takes it by the wide command. The game reads no caller-saved register after
    // the call.
    Xbyak::Label done;
    const void* start = c.getCurr();
    c.mov(edi, r15d);
    c.call(code(SetVolume));
    c.mov(rax, reinterpret_cast<u64>(&g_volumes));
    c.mov(eax, dword[rax + offsetof(Volumes, wide)]);
    c.test(eax, eax);
    c.jz(done);
    c.cmp(eax, dword[rbp + StartedVolume]);
    c.jne(done);
    c.sub(rsp, 16);
    c.mov(dword[rsp], eax);
    c.mov(edi, r15d);
    c.mov(esi, WideVolume);
    c.mov(rdx, rsp);
    c.mov(ecx, 1);
    c.lea(r8, ptr[rip + code(VolumeName)]);
    c.call(code(SoundCommand));
    c.add(rsp, 16);
    c.L(done);
    c.jmp(code(StartVolume.address + StartVolume.size));

    if (Hooks::Replace(module_base, AlertVolume, c, capacity, alert) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, StartVolume, c, capacity, start) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the low energy alert's sound could not be hooked");
        return false;
    }
    return true;
}

void Set(u32 position) {
    const float volume = static_cast<float>(std::min(position, TopPosition)) / OwnPosition;
    std::atomic_ref<float>(g_volumes.volume).store(volume, std::memory_order_relaxed);
    std::atomic_ref<float>(g_volumes.wide)
        .store(volume > 1.0f ? volume : 0.0f, std::memory_order_relaxed);
}

} // namespace Gr2Port::LowEnergyAlert
