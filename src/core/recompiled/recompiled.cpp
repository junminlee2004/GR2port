// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "common/arch.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/recompiled/recompiled.h"
#include "core/signals.h"

#ifdef ENABLE_RECOMPILED_MODE
#include <sys/mman.h>
#include "core/recompiled/build.h"

// The runtime's interface holds its API only where HAVE_GUEST_CPU is set; Linux hosts set it.
#define HAVE_GUEST_CPU 1
#include <guest_cpu.h>

// What the runtime needs from its host.
extern "C" {

// The guest image's base, for the runtime's fault reports.
uint64_t guest_image_base = 0;

// Guest stacks for calls from threads that have none, in the guest's address space.
void* runtime_low_map(size_t size, int prot) {
    using Core::MemoryProt;
    MemoryProt guest_prot = MemoryProt::NoAccess;
    if ((prot & PROT_READ) != 0) {
        guest_prot |= MemoryProt::CpuRead;
    }
    if ((prot & PROT_WRITE) != 0) {
        guest_prot |= MemoryProt::CpuWrite;
    }
    void* address = nullptr;
    if (Core::Memory::Instance()->MapMemory(&address, 0, size, guest_prot,
                                            Core::MemoryMapFlags::NoFlags, Core::VMAType::Stack,
                                            "Recompiled stack") != 0) {
        return nullptr;
    }
    return address;
}
}
#endif

namespace Core::Recompiled {

namespace {

bool g_enabled = false;
// The executable's library, which the others are beside.
std::string g_library;

#ifdef ENABLE_RECOMPILED_MODE
// The code the host writes for guest code to run: the entry's stack setup and aerolib's stubs.
constexpr u64 GuestCodeArenaSize = 64_KB;
u8* g_arena = nullptr;
u64 g_arena_used = 0;
std::mutex g_arena_mutex;

// The runtime reads the excluded ranges as it loads the library and keeps a table of 64, which
// aborts when full. The HUD switches are excluded again with each settings change.
bool g_loaded = false;

// A fiber's host stack: the recompiled code takes a frame of it for each guest call.
constexpr u64 FiberStackSize = 8_MB;
// Below it, unmapped: a page on any host.
constexpr u64 FiberGuardSize = 16_KB;
#endif

} // namespace

#if defined(ENABLE_RECOMPILED_MODE) && defined(__x86_64__)
// Saves the callee-saved registers on the calling stack, stores its pointer in *save, and goes on
// with the registers saved on the stack at load.
extern "C" void RecompiledSwitchStack(void** save, void* load);
// A fiber's first code on its own stack: calls entry (r13) with arg (r12).
extern "C" void RecompiledFiberStart();
asm(R"(
.global RecompiledSwitchStack
RecompiledSwitchStack:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

.global RecompiledFiberStart
RecompiledFiberStart:
    movq %r12, %rdi
    callq *%r13
    ud2
)");
#elif defined(ENABLE_RECOMPILED_MODE) && defined(__APPLE__) && defined(__aarch64__)
// Saves the callee-saved registers (x19-x30 and d8-d15, 160 bytes) on the calling stack, stores
// its pointer in *save, and goes on with the registers saved on the stack at load.
extern "C" void RecompiledSwitchStack(void** save, void* load);
// A fiber's first code on its own stack: calls entry (x20) with arg (x19).
extern "C" void RecompiledFiberStart();
asm(R"(
.text
.p2align 2
.globl _RecompiledSwitchStack
_RecompiledSwitchStack:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret

.globl _RecompiledFiberStart
_RecompiledFiberStart:
    mov x0, x19
    blr x20
    brk #1
)");
#endif

void Configure() {
#ifdef ENABLE_RECOMPILED_MODE
    std::string library = EmulatorSettings.GetRecompiledLibrary();
#ifndef ARCH_X86_64
    // Without the setting, the libraries made from the game on this machine (build.h).
    if (library.empty()) {
        library = Common::FS::PathToUTF8String(Build::Folder() / "librecomp.so");
    }
#endif
    if (library.empty()) {
        return;
    }
    g_library = library;
    // The runtime loads the library this names.
    setenv("GUEST_RECOMP_LIB", library.c_str(), 1);
    void* arena = mmap(nullptr, GuestCodeArenaSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_MSG(arena != MAP_FAILED, "Recompiled mode: no memory for guest code");
    g_arena = static_cast<u8*>(arena);
    guest_cpu_add_guest_code(reinterpret_cast<uintptr_t>(g_arena), GuestCodeArenaSize);
    g_enabled = true;
    LOG_INFO(Core, "Recompiled mode, with {}", library);
#endif
}

bool Enabled() {
    return g_enabled;
}

const std::string& Library() {
    return g_library;
}

#ifdef ENABLE_RECOMPILED_MODE

void AddGuestCode(VAddr start, u64 size) {
    LOG_INFO(Core, "Recompiled mode: guest code at {:#x}, {:#x} bytes", start, size);
    guest_cpu_add_guest_code(start, size);
}

bool IsGuestCode(VAddr address) {
    return guest_cpu_is_guest(address) != 0;
}

void Exclude(VAddr address, u64 size) {
    if (!g_loaded) {
        guest_cpu_recomp_exclude(address, size);
    }
}

void Invalidate(VAddr address, u64 size) {
    guest_cpu_invalidate(address, size);
}

void CheckWrite(VAddr address, u64 size) {
    if (!g_loaded || guest_cpu_recomp_code_checked(address, size) != 0) {
        return;
    }
    LOG_WARNING(Core,
                "Recompiled mode: the code written at {:#x} (GoldHEN {:#x}, {} bytes) has no hook "
                "point in the recompiled C, so the code holding it runs in the translator",
                address, address - guest_image_base + 0x400000, size);
    guest_cpu_recomp_drop(address, size);
}

void AddHookCode(VAddr start, u64 size) {
    guest_cpu_recomp_hook_code(start, size);
}

void LoadHookLibrary() {
    const std::filesystem::path folder = std::filesystem::path{g_library}.parent_path();
    guest_cpu_recomp_load_hook_code((folder / "hooks.so").string().c_str());
    // And a library for each hook code that settings give, which the builds on this machine make
    // (build.h).
    std::vector<std::filesystem::path> libraries;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{folder / "hooks", ec}) {
        if (entry.path().extension() == ".so") {
            libraries.push_back(entry.path());
        }
    }
    std::ranges::sort(libraries);
    for (const std::filesystem::path& library : libraries) {
        guest_cpu_recomp_load_hook_code(library.string().c_str());
    }
    if (guest_cpu_recomp_hook_fits() != 0) {
        return;
    }
    LOG_WARNING(Core, "Recompiled mode: no library of the hook code was made from this hook code, "
                      "and what none of them holds runs in the interpreter");
    if (const std::optional<std::filesystem::path> dump = Build::HookDump()) {
        guest_cpu_recomp_dump_hook_code(dump->string().c_str());
        Build::RestartForHookCode();
    }
}

static_assert(sizeof(HookCpu) == sizeof(GuestHookCpu) &&
              offsetof(HookCpu, rip) == offsetof(GuestHookCpu, rip) &&
              offsetof(HookCpu, v) == offsetof(GuestHookCpu, v));

bool HookEntry(VAddr address, EntryHook hook, void* data, VAddr unhooked) {
    return g_loaded && guest_cpu_recomp_hook_entry(address, reinterpret_cast<GuestEntryHook>(hook),
                                                   data, unhooked) != 0;
}

void CallHost(HookCpu* cpu, const void* function) {
    guest_cpu_hook_call_host(reinterpret_cast<GuestHookCpu*>(cpu), function);
}

VAddr GuestReturn(u64 value) {
    return guest_cpu_guest_return(value);
}

void LoadLibrary(VAddr image_base) {
    // Native mode's CPU patches create the fault dispatcher, which installs the signal handlers,
    // as the first module loads. Recompiled mode has no CPU patches, so it creates it here.
    Signals::Instance();
    guest_image_base = image_base;
    guest_cpu_recomp_load(image_base);
    g_loaded = true;
    LOG_INFO(Core, "Recompiled mode: {} functions run from the recompiled C",
             guest_cpu_recomp_functions());
}

void LoadModuleLibrary(const std::string& name, VAddr image_base) {
    const std::filesystem::path library =
        std::filesystem::path{g_library}.parent_path() / (name + ".so");
    guest_cpu_recomp_load_module(library.string().c_str(), image_base, name.c_str());
    LOG_INFO(Core, "Recompiled mode: {} loaded, {} functions run from the recompiled C", name,
             guest_cpu_recomp_functions());
}

void SetThreadPointer(void* tcb) {
    guest_cpu_set_thread_pointer(reinterpret_cast<uintptr_t>(tcb));
}

bool HandleAlignmentFault(void* context) {
    return guest_cpu_handle_alignment_fault(context) != 0;
}

void GetFpControl(u32* mxcsr, u16* fcw) {
    guest_cpu_get_fp_control(mxcsr, fcw);
}

void SetFpControl(u32 mxcsr, u16 fcw) {
    guest_cpu_set_fp_control(mxcsr, fcw);
}

void CallGuest(VAddr function, GuestRegisters& registers) {
    guest_cpu_call_full(function, registers.gpr, registers.xmm, registers.out_gpr,
                        registers.out_xmm);
}

void CallGuestWithStackArgument(VAddr function, GuestRegisters& registers) {
    guest_cpu_call_full_stack(function, registers.gpr, registers.xmm, &registers.stack_argument, 1,
                              registers.out_gpr, registers.out_xmm);
}

void* RunThread(VAddr function, void* arg, void* stack, u64 stack_size) {
    const u64 argument = reinterpret_cast<u64>(arg);
    return reinterpret_cast<void*>(
        guest_cpu_call_on_stack(function, 1, &argument, stack, stack_size));
}

void RunEntry(VAddr entry, const void* params, const void* exit_function, void* stack,
              u64 stack_size) {
    // The stack as native mode's RunMainEntry gives it to the entry: the top aligned to 16 bytes,
    // 8 bytes left free, then the second and the first word of params, where rsp points. Nothing
    // goes on it for a return. rdi = params and rsi = exit_function.
    const u64 top = reinterpret_cast<u64>(stack) + stack_size;
    u64* const rsp = reinterpret_cast<u64*>((top & ~u64{15}) - 24);
    std::memcpy(rsp, params, 2 * sizeof(u64));
    const std::array<u64, 2> args{reinterpret_cast<u64>(params),
                                  reinterpret_cast<u64>(exit_function)};
    guest_cpu_start_on_stack(entry, static_cast<int>(args.size()), args.data(),
                             reinterpret_cast<u64>(rsp), stack, stack_size);
    UNREACHABLE_MSG("The executable's entry returned");
}

u8* AllocateGuestCode(u64 size) {
    std::scoped_lock lock{g_arena_mutex};
    const u64 start = (g_arena_used + 15) & ~u64{15};
    ASSERT_MSG(g_arena != nullptr && start + size <= GuestCodeArenaSize,
               "Recompiled mode: the guest code arena is full");
    g_arena_used = start + size;
    return g_arena + start;
}

VAddr ImportReturn() {
    return guest_cpu_import_return();
}

VAddr GuestCaller(const void* return_address) {
    return Enabled() ? ImportReturn() : reinterpret_cast<VAddr>(return_address);
}

void RunFiber(VAddr entry, u64 arg_on_initialize, u64 arg_on_run_to, void* stack, u64 stack_size) {
    const std::array<u64, 2> args{arg_on_initialize, arg_on_run_to};
    guest_cpu_call_on_stack(entry, static_cast<int>(args.size()), args.data(), stack, stack_size);
}

HostFiber::HostFiber() : context{guest_cpu_context_create()} {}

HostFiber::HostFiber(void (*entry)(void*), void* arg) : context{guest_cpu_context_create()} {
    stack_size = FiberStackSize + FiberGuardSize;
    stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    ASSERT_MSG(stack != MAP_FAILED, "Recompiled mode: no memory for a fiber's host stack");
    // The lowest pages fault when the stack overflows.
    ASSERT(mprotect(stack, FiberGuardSize, PROT_NONE) == 0);
    u64* const top = static_cast<u64*>(stack) + stack_size / sizeof(u64);
#if defined(__x86_64__)
    // What the first switch to the fiber pops: r15, r14, r13 = entry, r12 = arg, rbx, rbp and the
    // return address. FiberStart then calls entry with the stack aligned as a call needs.
    u64* const frame = top - 9;
    frame[2] = reinterpret_cast<u64>(entry);
    frame[3] = reinterpret_cast<u64>(arg);
    frame[6] = reinterpret_cast<u64>(&RecompiledFiberStart);
#else
    // What the first switch to the fiber loads: x19 = arg, x20 = entry and x30, where it returns
    // to, in a frame of 20 registers that leaves the stack aligned to 16 bytes.
    u64* const frame = top - 20;
    frame[0] = reinterpret_cast<u64>(arg);
    frame[1] = reinterpret_cast<u64>(entry);
    frame[11] = reinterpret_cast<u64>(&RecompiledFiberStart);
#endif
    stack_pointer = frame;
}

HostFiber::~HostFiber() {
    if (stack != nullptr) {
        munmap(stack, stack_size);
    }
    guest_cpu_context_destroy(static_cast<GuestContext*>(context));
}

void HostFiber::SwitchTo(HostFiber& to) {
    guest_cpu_context_switch(static_cast<GuestContext*>(context),
                             static_cast<GuestContext*>(to.context));
    RecompiledSwitchStack(&stack_pointer, to.stack_pointer);
}

#else

void AddGuestCode(VAddr, u64) {}

bool IsGuestCode(VAddr) {
    return false;
}

void Exclude(VAddr, u64) {}

void Invalidate(VAddr, u64) {}

void CheckWrite(VAddr, u64) {}

void AddHookCode(VAddr, u64) {}

void LoadHookLibrary() {}

bool HookEntry(VAddr, EntryHook, void*, VAddr) {
    return false;
}

void CallHost(HookCpu*, const void*) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

VAddr GuestReturn(u64 value) {
    return value;
}

void LoadLibrary(VAddr) {}

void LoadModuleLibrary(const std::string&, VAddr) {}

void SetThreadPointer(void*) {}

bool HandleAlignmentFault(void*) {
    return false;
}

void GetFpControl(u32* mxcsr, u16* fcw) {
    *mxcsr = 0x1f80;
    *fcw = 0x37f;
}

void SetFpControl(u32, u16) {}

void CallGuest(VAddr, GuestRegisters&) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

void CallGuestWithStackArgument(VAddr, GuestRegisters&) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

void* RunThread(VAddr, void*, void*, u64) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

void RunFiber(VAddr, u64, u64, void*, u64) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

HostFiber::HostFiber() = default;

HostFiber::HostFiber(void (*)(void*), void*) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

HostFiber::~HostFiber() = default;

void HostFiber::SwitchTo(HostFiber&) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

void RunEntry(VAddr, const void*, const void*, void*, u64) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

u8* AllocateGuestCode(u64) {
    UNREACHABLE_MSG("Recompiled mode is not in this build");
}

VAddr ImportReturn() {
    return 0;
}

VAddr GuestCaller(const void* return_address) {
    return reinterpret_cast<VAddr>(return_address);
}

#endif

} // namespace Core::Recompiled
