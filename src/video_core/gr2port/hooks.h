// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/types.h"

namespace Xbyak {
class CodeGenerator;
}

// Observers and replacements of functions of the Gravity Rush 2 executable. An observer puts a
// jump to a stub at a function's entry, and the function still runs: when the entry holds the
// instructions the hook expects, the stub ends in a copy of them and a jump past them; when it
// holds the jump of an earlier hook, the stub continues at that hook. A replacement puts a jump
// to a native function at the entry, or a return. Stubs are written by c within capacity bytes
// of its buffer, which a rel32 jump from the executable reaches.
namespace Gr2Port::Hooks {

/// A function entry: its GoldHEN address and the instructions that cover its first 5 bytes,
/// which address nothing relative to rip.
struct Entry {
    u64 address;
    u8 size;
    std::array<u8, 11> bytes;
};

/// push rbp; mov rbp, rsp; push r15.
inline constexpr std::array<u8, 11> FramePrologue{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
/// push rbp; mov rbp, rsp; push r14.
inline constexpr std::array<u8, 11> FramePrologueR14{0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};

/// The registers at the entry of a hooked call, from the lowest stack address up: the low 64
/// bits of xmm0, the caller-saved general registers, then the call's return address.
struct Registers {
    u64 xmm0;
    u64 r11;
    u64 r10;
    u64 rax;
    u64 r9;
    u64 r8;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
    u64 ret;
};

/// The argument registers of a hooked call, from the lowest stack address up.
struct Arguments {
    u64 r9;
    u64 r8;
    u64 rcx;
    u64 rdx;
    u64 rsi;
    u64 rdi;
};

/// Runs before a hooked function, with the frame pointer of its caller.
using EntryObserver = void PS4_SYSV_ABI (*)(u32 id, const Registers* registers, u64 frame_pointer);

/// Runs after a hooked function returns, with the arguments it was called with and its result.
using ReturnObserver = void PS4_SYSV_ABI (*)(u32 id, const Arguments* arguments, u64 result);

enum class Result {
    Hooked,
    // Hooked ahead of an earlier hook.
    Chained,
    // The entry holds neither the expected instructions nor a jump.
    Mismatch,
    NoRoom,
};

/// Calls observer(id, ...) at each call of the function at entry, before it runs. The observer
/// sees every general register as the caller left it.
Result ObserveEntry(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
                    EntryObserver observer, u32 id);

/// Calls observer(id, ...) when each call of the function at entry returns. The function takes
/// its arguments in registers and in at most StackArguments stack slots, and returns in rax and
/// rdx; the observer leaves both as the function returned them.
Result ObserveReturn(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c,
                     size_t capacity, ReturnObserver observer, u32 id);

inline constexpr u32 StackArguments = 2;

/// Makes each call of the function at entry run target in its place, with every register as the
/// caller left it: target takes the function's arguments and returns its result. The jump to
/// target is written to c. With original, c also gets a trampoline that runs the function's own
/// code, and original receives its address. The entry must hold the instructions the entry
/// expects.
Result Replace(VAddr module_base, const Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
               const void* target, const void** original = nullptr);

/// Makes the function at entry return at once, so its own code no longer runs. The entry must
/// hold the instructions the entry expects.
Result Return(VAddr module_base, const Entry& entry);

/// Any thread, while the game runs: makes the function at entry return at once or run its own code
/// again, by writing its first byte. The other bytes must be the ones the entry expects.
Result SetReturning(VAddr module_base, const Entry& entry, bool returning);

} // namespace Gr2Port::Hooks
