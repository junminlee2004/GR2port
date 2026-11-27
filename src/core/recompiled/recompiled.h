// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bit>
#include <cstring>
#include <string>
#include <type_traits>
#include "common/types.h"

// Recompiled mode: the game runs from the C recompiled from its executable, and a CPU runtime
// executes every other guest instruction (an interpreter on x86-64, a translator on arm64). No
// guest instruction runs natively then, so each call from host code into guest code goes through
// Call or one of the entries below. In native mode they call the guest code directly.
namespace Core::Recompiled {

/// Reads the settings, before any thread runs. Recompiled mode is on when the build has it and
/// recompiled_library names the recompiled C's library.
void Configure();

/// Whether the game runs in recompiled mode.
bool Enabled();

/// Recompiled mode: [start, start + size) holds guest code, which only the runtime executes.
void AddGuestCode(VAddr start, u64 size);

/// Whether address lies in guest code.
bool IsGuestCode(VAddr address);

/// Recompiled mode, before LoadLibrary: keeps the function at [address, address + size) out of
/// the recompiled C, so that writes to its code while the game runs take effect. A range given
/// after LoadLibrary changes nothing.
void Exclude(VAddr address, u64 size);

/// Recompiled mode: makes a write to guest code at [address, address + size) take effect.
void Invalidate(VAddr address, u64 size);

/// Recompiled mode, after LoadLibrary and before a write to the executable's code at
/// [address, address + size): the recompiled C holding that code without a hook point there,
/// which reads it as it runs, leaves it to the runtime, so that the write takes effect.
void CheckWrite(VAddr address, u64 size);

/// Recompiled mode: [start, start + size) holds hook code of the host (stubs that hooked code
/// jumps to), which the recompiled C's hook points run where the code was hooked.
void AddHookCode(VAddr start, u64 size);

/// Recompiled mode, after the hook code is written and before guest code runs it: loads the hook
/// code's recompiled C, from hooks.so next to the executable's library, when that file exists.
/// Hook code it does not hold runs in the runtime's interpreter.
void LoadHookLibrary();

/// The guest registers a hook of a recompiled function sees.
struct HookCpu {
    enum Register : u32 { Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11 };
    u64 r[16];
    u64 rip;
    u64 flags;
    u8 v[16][32];
};

/// A hook at a recompiled function's entry. It runs at each call of the function, with the
/// registers as the caller left them, and returns with rip at the call's return address.
/// function(cpu) runs the function and returns once the function has returned.
using EntryHook = void (*)(HookCpu* cpu, void* data, void (*function)(HookCpu* cpu));

/// Recompiled mode, after LoadLibrary: makes each call of the recompiled function at address run
/// hook(cpu, data, ...) in its place. If unhooked is not 0, it is guest code the game does not
/// run, and a call of it runs the function as the function ran before. False when no recompiled
/// function starts at address.
bool HookEntry(VAddr address, EntryHook hook, void* data, VAddr unhooked);

/// In an entry hook: runs the host function as the call of the hook, with the guest registers
/// as its arguments and results, and returns to the call's return address.
void CallHost(HookCpu* cpu, const void* function);

/// The guest return address that a value on the guest stack stands for.
VAddr GuestReturn(u64 value);

/// Recompiled mode: loads the recompiled C for the executable at image_base. It runs once, after
/// the executable is patched and before it is hooked and guest code runs.
void LoadLibrary(VAddr image_base);

/// Recompiled mode, after LoadLibrary: loads the recompiled C of the game module name, loaded at
/// image_base, from name.so next to the executable's library, when that file exists.
void LoadModuleLibrary(const std::string& name, VAddr image_base);

/// Recompiled mode: gives the calling thread its guest thread pointer.
void SetThreadPointer(void* tcb);

/// In a bus error's handler, with its context: whether the fault is an unaligned access of the
/// runtime's translated code (arm64), which the call then makes.
bool HandleAlignmentFault(void* context);

/// Recompiled mode: the floating-point control words of the calling thread's guest code.
void GetFpControl(u32* mxcsr, u16* fcw);

/// Recompiled mode: gives the calling thread these floating-point control words.
void SetFpControl(u32 mxcsr, u16 fcw);

/// The registers of a call into guest code: integer arguments in rdi, rsi, rdx, rcx, r8 and r9
/// (a seventh in stack_argument), vector ones in xmm0-7, results in rax, rdx, xmm0 and xmm1.
struct GuestRegisters {
    u64 gpr[6];
    u64 stack_argument;
    u8 xmm[8][16];
    u64 out_gpr[2];
    u8 out_xmm[2][16];
};

/// Recompiled mode: calls the guest function at function with registers.
void CallGuest(VAddr function, GuestRegisters& registers);

/// Recompiled mode: calls the guest function at function with registers and a seventh integer
/// argument, registers.stack_argument, which the call puts on the stack.
void CallGuestWithStackArgument(VAddr function, GuestRegisters& registers);

/// Recompiled mode: runs a guest thread's start routine on its guest stack and returns its
/// result.
void* RunThread(VAddr function, void* arg, void* stack, u64 stack_size);

/// Recompiled mode: starts the executable's entry on the guest stack the kernel would give it:
/// params' first two words on top, params in rdi and exit_function in rsi.
[[noreturn]] void RunEntry(VAddr entry, const void* params, const void* exit_function, void* stack,
                           u64 stack_size);

/// Recompiled mode: a stub of `size` bytes in guest code, for code the host writes.
u8* AllocateGuestCode(u64 size);

/// Recompiled mode, in a host function guest code called: the guest return address of that call
/// (past the guest's call instruction), or 0 when the thread is in no such call.
VAddr ImportReturn();

/// In a host function guest code called, given its own return address: the guest return address
/// of the call. Natively that is the return address; in recompiled mode the runtime makes the call
/// and keeps the guest's on the guest stack.
VAddr GuestCaller(const void* return_address);

/// Recompiled mode: runs a fiber's entry with its two arguments on its guest stack. Returns
/// only when the entry returns.
void RunFiber(VAddr entry, u64 arg_on_initialize, u64 arg_on_run_to, void* stack, u64 stack_size);

/// Recompiled mode: a side that guest code switches away from in the middle of a call and back
/// to, on one host thread at a time (a fiber, or the thread that runs fibers). A fiber has a host
/// stack and a guest CPU state of its own.
class HostFiber {
public:
    /// The calling thread's own side, which its fibers switch back to.
    HostFiber();
    /// A fiber, which runs entry(arg) on its own host stack from the first switch to it on.
    HostFiber(void (*entry)(void*), void* arg);
    ~HostFiber();
    HostFiber(const HostFiber&) = delete;
    HostFiber& operator=(const HostFiber&) = delete;

    /// Leaves this side, the calling one, for to. Returns when a switch comes back to it.
    void SwitchTo(HostFiber& to);

private:
    void* stack = nullptr;
    u64 stack_size = 0;
    void* stack_pointer = nullptr;
    void* context = nullptr;
};

namespace Detail {

template <typename T>
inline constexpr bool IsVector = std::is_floating_point_v<T>;

template <typename T>
void Pass(GuestRegisters& registers, u32& gpr, u32& xmm, T value) {
    if constexpr (IsVector<T>) {
        std::memcpy(registers.xmm[xmm++], &value, sizeof(value));
    } else {
        u64 word;
        if constexpr (std::is_pointer_v<T>) {
            word = reinterpret_cast<u64>(value);
        } else {
            static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "unsupported guest argument");
            word = static_cast<u64>(value);
        }
        if (gpr < 6) {
            registers.gpr[gpr] = word;
        } else {
            registers.stack_argument = word;
        }
        ++gpr;
    }
}

template <typename R>
R Result(const GuestRegisters& registers) {
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (IsVector<R>) {
        R value;
        std::memcpy(&value, registers.out_xmm[0], sizeof(value));
        return value;
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>(registers.out_gpr[0]);
    } else if constexpr (std::is_same_v<R, bool>) {
        return (registers.out_gpr[0] & 0xff) != 0;
    } else {
        static_assert(std::is_integral_v<R> || std::is_enum_v<R>, "unsupported guest result");
        return static_cast<R>(registers.out_gpr[0]);
    }
}

} // namespace Detail

/// Calls function: guest code in recompiled mode through the runtime, otherwise directly.
template <typename R, typename... Args>
R Call(PS4_SYSV_ABI R (*function)(Args...), std::type_identity_t<Args>... args) {
    if (!Enabled() || !IsGuestCode(reinterpret_cast<VAddr>(function))) {
        return function(args...);
    }
    constexpr u32 integers = ((Detail::IsVector<Args> ? 0 : 1) + ... + 0);
    constexpr u32 vectors = sizeof...(Args) - integers;
    static_assert(integers <= 7, "guest calls take at most 7 integer arguments");
    static_assert(vectors <= 8, "guest calls take at most 8 vector arguments");
    GuestRegisters registers{};
    u32 gpr = 0;
    u32 xmm = 0;
    (Detail::Pass(registers, gpr, xmm, args), ...);
    if constexpr (integers > 6) {
        CallGuestWithStackArgument(reinterpret_cast<VAddr>(function), registers);
    } else {
        CallGuest(reinterpret_cast<VAddr>(function), registers);
    }
    return Detail::Result<R>(registers);
}

} // namespace Core::Recompiled
