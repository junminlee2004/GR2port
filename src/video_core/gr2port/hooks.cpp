// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include <xbyak/xbyak.h>

#include "common/memory_patcher.h"
#include "video_core/gr2port/hooks.h"

namespace Gr2Port::Hooks {

namespace {

// Addresses are GoldHEN patch addresses: the executable's first segment at 0x400000.
constexpr u64 CodeBase = 0x400000;
constexpr u32 JmpSize = 5;
constexpr u8 ReturnOpcode = 0xc3;
// The longest stub and trampoline.
constexpr size_t MaxStubBytes = 192;

struct Site {
    u8* entry;
    const u8* next;
    bool chained;
};

// Finds where a stub for entry continues: the earlier hook a jump at the entry leads to, or a
// trampoline written to c that runs the covered instructions and jumps past them.
Result Prepare(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
               Site& site) {
    if (entry.address < CodeBase ||
        entry.address - CodeBase + entry.size > MemoryPatcher::g_eboot_image_size) {
        return Result::Mismatch;
    }
    if (capacity - c.getSize() < MaxStubBytes) {
        return Result::NoRoom;
    }
    site.entry = reinterpret_cast<u8*>(module_base + entry.address - CodeBase);
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
    if (entry.address < CodeBase ||
        entry.address - CodeBase + entry.size > MemoryPatcher::g_eboot_image_size) {
        return nullptr;
    }
    u8* const site = reinterpret_cast<u8*>(module_base + entry.address - CodeBase);
    return std::memcmp(site, entry.bytes.data(), entry.size) == 0 ? site : nullptr;
}

void Patch(const Site& site, const Entry& entry, const u8* stub) {
    const auto displacement = static_cast<s32>(stub - (site.entry + JmpSize));
    site.entry[0] = 0xe9;
    std::memcpy(site.entry + 1, &displacement, sizeof(displacement));
    if (!site.chained) {
        std::memset(site.entry + JmpSize, 0xcc, entry.size - JmpSize);
    }
}

} // namespace

Result ObserveEntry(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
                    EntryObserver observer, u32 id) {
    using namespace Xbyak::util;
    Site site{};
    const Result result = Prepare(module_base, entry, c, capacity, site);
    if (result != Result::Hooked && result != Result::Chained) {
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
    Site site{};
    const Result result = Prepare(module_base, entry, c, capacity, site);
    if (result != Result::Hooked && result != Result::Chained) {
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
    site[0] = ReturnOpcode;
    return Result::Hooked;
}

Result SetReturning(VAddr module_base, const Entry& entry, bool returning) {
    if (entry.address < CodeBase || entry.size == 0 ||
        entry.address - CodeBase + entry.size > MemoryPatcher::g_eboot_image_size) {
        return Result::Mismatch;
    }
    u8* const site = reinterpret_cast<u8*>(module_base + entry.address - CodeBase);
    if ((site[0] != entry.bytes[0] && site[0] != ReturnOpcode) ||
        std::memcmp(site + 1, entry.bytes.data() + 1, entry.size - 1) != 0) {
        return Result::Mismatch;
    }
    site[0] = returning ? ReturnOpcode : entry.bytes[0];
    return Result::Hooked;
}

} // namespace Gr2Port::Hooks
