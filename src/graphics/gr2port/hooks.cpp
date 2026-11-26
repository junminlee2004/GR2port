// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include <xbyak/xbyak.h>

#include "common/memory_patcher.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::Hooks {

namespace {

constexpr u32 JmpSize = 5;
constexpr u8 ReturnOpcode = 0xc3;

struct Site {
    u8* entry;
    const u8* next;
    bool chained;
};

// The entry's bytes in the executable, or null when they lie outside it.
u8* At(VAddr module_base, const Entry& entry) {
    if (entry.address < CodeBase ||
        entry.address - CodeBase + entry.size > MemoryPatcher::g_eboot_image_size) {
        return nullptr;
    }
    return reinterpret_cast<u8*>(module_base + entry.address - CodeBase);
}

// Finds where a stub for entry continues: the earlier hook a jump at the entry leads to, or a
// trampoline written to c that runs the covered instructions and jumps past them.
Result Prepare(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
               Site& site) {
    site.entry = At(module_base, entry);
    if (site.entry == nullptr) {
        return Result::Mismatch;
    }
    if (capacity - c.getSize() < MaxStubBytes) {
        return Result::NoRoom;
    }
    site.chained = site.entry[0] == 0xe9;
    if (site.chained) {
        s32 displacement;
        std::memcpy(&displacement, site.entry + 1, sizeof(displacement));
        site.next = site.entry + JmpSize + displacement;
        return Result::Chained;
    }
    if (std::memcmp(site.entry, entry.bytes.data(), entry.size) != 0) {
        return Result::Mismatch;
    }
    site.next = c.getCurr();
    for (u32 i = 0; i < entry.size; i++) {
        c.db(site.entry[i]);
    }
    c.jmp(site.entry + entry.size);
    return Result::Hooked;
}

// The function's entry, when it holds the instructions the entry expects.
u8* Expected(VAddr module_base, const Entry& entry) {
    u8* const site = At(module_base, entry);
    return site != nullptr && std::memcmp(site, entry.bytes.data(), entry.size) == 0 ? site
                                                                                     : nullptr;
}

void Patch(const Site& site, const Entry& entry, const u8* stub) {
    const auto displacement = static_cast<s32>(stub - (site.entry + JmpSize));
    Core::Recompiled::CheckWrite(reinterpret_cast<VAddr>(site.entry), entry.size);
    site.entry[0] = 0xe9;
    std::memcpy(site.entry + 1, &displacement, sizeof(displacement));
    if (!site.chained) {
        std::memset(site.entry + JmpSize, 0xcc, entry.size - JmpSize);
    }
}

// Recompiled mode: hooks of a recompiled function's entry that observe or replace its calls
// without a write to its code. The other hooks write the code, which the recompiled C runs from
// its hook points.
struct EntryHookData {
    EntryObserver entry_observer;
    ReturnObserver return_observer;
    const void* target;
    u32 id;
};

using Core::Recompiled::HookCpu;

void RunEntryObserver(HookCpu* cpu, void* data, void (*function)(HookCpu* cpu)) {
    const auto& hook = *static_cast<const EntryHookData*>(data);
    Registers registers{};
    std::memcpy(&registers.xmm0, cpu->v[0], sizeof(registers.xmm0));
    registers.r11 = cpu->r[HookCpu::R11];
    registers.r10 = cpu->r[HookCpu::R10];
    registers.rax = cpu->r[HookCpu::Rax];
    registers.r9 = cpu->r[HookCpu::R9];
    registers.r8 = cpu->r[HookCpu::R8];
    registers.rcx = cpu->r[HookCpu::Rcx];
    registers.rdx = cpu->r[HookCpu::Rdx];
    registers.rsi = cpu->r[HookCpu::Rsi];
    registers.rdi = cpu->r[HookCpu::Rdi];
    registers.ret =
        Core::Recompiled::GuestReturn(*reinterpret_cast<const u64*>(cpu->r[HookCpu::Rsp]));
    hook.entry_observer(hook.id, &registers, cpu->r[HookCpu::Rbp]);
    function(cpu);
}

void RunReturnObserver(HookCpu* cpu, void* data, void (*function)(HookCpu* cpu)) {
    const auto& hook = *static_cast<const EntryHookData*>(data);
    const Arguments arguments{cpu->r[HookCpu::R9],  cpu->r[HookCpu::R8],  cpu->r[HookCpu::Rcx],
                              cpu->r[HookCpu::Rdx], cpu->r[HookCpu::Rsi], cpu->r[HookCpu::Rdi]};
    function(cpu);
    hook.return_observer(hook.id, &arguments, cpu->r[HookCpu::Rax]);
}

void RunReplacement(HookCpu* cpu, void* data, void (*)(HookCpu* cpu)) {
    Core::Recompiled::CallHost(cpu, static_cast<const EntryHookData*>(data)->target);
}

// Recompiled mode: whether hook runs at each call of the recompiled function at the entry, which
// holds the instructions the entry expects or the jump of an earlier hook. If unhooked is not
// null, a call of it runs the function as it ran before.
bool HookRecompiled(VAddr module_base, const Entry& entry, Core::Recompiled::EntryHook hook,
                    const EntryHookData& data, const void* unhooked) {
    const u8* const site = At(module_base, entry);
    if (!Core::Recompiled::Enabled() || site == nullptr ||
        (site[0] != 0xe9 && std::memcmp(site, entry.bytes.data(), entry.size) != 0)) {
        return false;
    }
    auto* const kept = new EntryHookData{data};
    if (!Core::Recompiled::HookEntry(reinterpret_cast<VAddr>(site), hook, kept,
                                     reinterpret_cast<VAddr>(unhooked))) {
        delete kept;
        return false;
    }
    return true;
}

} // namespace

bool Holds(VAddr module_base, const Entry& entry) {
    return Expected(module_base, entry) != nullptr;
}

Result ObserveEntry(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
                    EntryObserver observer, u32 id) {
    using namespace Xbyak::util;
    if (HookRecompiled(
            module_base, entry, &RunEntryObserver,
            {.entry_observer = observer, .return_observer = nullptr, .target = nullptr, .id = id},
            nullptr)) {
        return At(module_base, entry)[0] == 0xe9 ? Result::Chained : Result::Hooked;
    }
    Site site{};
    const Result result = Prepare(module_base, entry, c, capacity, site);
    if (!Hooked(result)) {
        return result;
    }
    const u8* stub = c.getCurr();
    // Saves the caller-saved general registers and xmm0 in the order of Registers, aligns the
    // stack for the call and continues with every register as the caller left it. No hooked
    // function takes a vector register argument other than xmm0.
    const std::array<Xbyak::Reg64, 9> saved{rdi, rsi, rdx, rcx, r8, r9, rax, r10, r11};
    for (const auto& reg : saved) {
        c.push(reg);
    }
    c.sub(rsp, 8);
    c.movq(ptr[rsp], xmm0);
    c.push(rbp);
    c.mov(rbp, rsp);
    c.and_(rsp, ~u32{15});
    c.mov(edi, id);
    c.lea(rsi, ptr[rbp + 8]);
    c.mov(rdx, ptr[rbp]);
    c.mov(rax, reinterpret_cast<u64>(observer));
    c.call(rax);
    c.mov(rsp, rbp);
    c.pop(rbp);
    c.movq(xmm0, ptr[rsp]);
    c.add(rsp, 8);
    for (auto reg = saved.rbegin(); reg != saved.rend(); ++reg) {
        c.pop(*reg);
    }
    c.jmp(site.next);
    Patch(site, entry, stub);
    return result;
}

Result ObserveReturn(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c,
                     size_t capacity, ReturnObserver observer, u32 id) {
    using namespace Xbyak::util;
    if (HookRecompiled(
            module_base, entry, &RunReturnObserver,
            {.entry_observer = nullptr, .return_observer = observer, .target = nullptr, .id = id},
            nullptr)) {
        return At(module_base, entry)[0] == 0xe9 ? Result::Chained : Result::Hooked;
    }
    Site site{};
    const Result result = Prepare(module_base, entry, c, capacity, site);
    if (!Hooked(result)) {
        return result;
    }
    const u8* stub = c.getCurr();
    // The frame: the argument registers in the order of Arguments, then copies of the stack
    // arguments, which the function finds above its return address. The entry stack is 8 off a
    // 16-byte boundary, so rbp and every call below are aligned.
    const std::array<Xbyak::Reg64, 6> arguments{rdi, rsi, rdx, rcx, r8, r9};
    constexpr s32 ArgumentBytes = 6 * 8;
    constexpr s32 CopyBytes = StackArguments * 8;
    static_assert((ArgumentBytes + CopyBytes) % 16 == 0);
    c.push(rbp);
    c.mov(rbp, rsp);
    for (const auto& reg : arguments) {
        c.push(reg);
    }
    c.sub(rsp, CopyBytes);
    for (u32 i = 0; i < StackArguments; i++) {
        c.mov(r11, ptr[rbp + 16 + i * 8]);
        c.mov(ptr[rsp + i * 8], r11);
    }
    for (u32 i = 0; i < arguments.size(); i++) {
        c.mov(arguments[i], ptr[rbp - 8 - static_cast<s32>(i) * 8]);
    }
    c.mov(r11, reinterpret_cast<u64>(site.next));
    c.call(r11);
    // The stack argument copies are dead: they hold the results across the observer.
    c.mov(ptr[rsp], rax);
    c.mov(ptr[rsp + 8], rdx);
    c.mov(edi, id);
    c.lea(rsi, ptr[rbp - ArgumentBytes]);
    c.mov(rdx, rax);
    c.mov(r11, reinterpret_cast<u64>(observer));
    c.call(r11);
    c.mov(rax, ptr[rsp]);
    c.mov(rdx, ptr[rsp + 8]);
    c.mov(rsp, rbp);
    c.pop(rbp);
    c.ret();
    Patch(site, entry, stub);
    return result;
}

Result Replace(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
               const void* target, const void** original) {
    u8* const site = Expected(module_base, entry);
    if (site == nullptr || entry.size < JmpSize) {
        return Result::Mismatch;
    }
    if (capacity - c.getSize() < MaxStubBytes) {
        return Result::NoRoom;
    }
    if (original != nullptr) {
        *original = c.getCurr();
        for (u32 i = 0; i < entry.size; i++) {
            c.db(site[i]);
        }
        c.jmp(site + entry.size);
    }
    // Recompiled mode: a host function replaces the calls of the recompiled function, and the
    // trampoline's run the function.
    if (!Core::Recompiled::IsGuestCode(reinterpret_cast<VAddr>(target)) &&
        HookRecompiled(
            module_base, entry, &RunReplacement,
            {.entry_observer = nullptr, .return_observer = nullptr, .target = target, .id = 0},
            original != nullptr ? *original : nullptr)) {
        return Result::Hooked;
    }
    // jmp [rip]: the target's address follows the instruction.
    const u8* stub = c.getCurr();
    c.db(0xff);
    c.db(0x25);
    c.dd(0);
    c.dq(reinterpret_cast<u64>(target));
    Patch({site, nullptr, false}, entry, stub);
    return Result::Hooked;
}

Result Return(VAddr module_base, const Entry& entry) {
    u8* const site = Expected(module_base, entry);
    if (site == nullptr || entry.size == 0) {
        return Result::Mismatch;
    }
    Core::Recompiled::CheckWrite(reinterpret_cast<VAddr>(site), entry.size);
    site[0] = ReturnOpcode;
    return Result::Hooked;
}

Result SetReturning(VAddr module_base, const Entry& entry, bool returning) {
    u8* const site = At(module_base, entry);
    if (site == nullptr || entry.size == 0) {
        return Result::Mismatch;
    }
    if ((site[0] != entry.bytes[0] && site[0] != ReturnOpcode) ||
        std::memcmp(site + 1, entry.bytes.data() + 1, entry.size - 1) != 0) {
        return Result::Mismatch;
    }
    // Recompiled mode: the recompiled C reads the byte at its hook point; without one, the runtime
    // runs the function and sees each write.
    const VAddr address = reinterpret_cast<VAddr>(site);
    Core::Recompiled::CheckWrite(address, entry.size);
    site[0] = returning ? ReturnOpcode : entry.bytes[0];
    Core::Recompiled::Invalidate(address, 1);
    return Result::Hooked;
}

} // namespace Gr2Port::Hooks
