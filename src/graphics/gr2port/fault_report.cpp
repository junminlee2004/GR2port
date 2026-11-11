// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <cwchar>
#include <string>

#include <fmt/format.h>
#include <windows.h>

#include "common/logging/log.h"
#include "common/types.h"
#include "graphics/gr2port/fault_report.h"

namespace Gr2Port {

namespace {

constexpr u32 MaxFrames = 48;

// "module+0xrva", or the bare address outside any module.
std::string Describe(u64 address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &module);
    wchar_t path[MAX_PATH]{};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return fmt::format("{:#x}", address);
    }
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    std::string name;
    for (const wchar_t* c = slash != nullptr ? slash + 1 : path; *c != 0; c++) {
        name += static_cast<char>(*c);
    }
    return fmt::format("{}+{:#x}", name, address - reinterpret_cast<u64>(module));
}

// The return addresses from the fault outward. A frame whose function has an exception handler
// is marked with *.
std::string Stack(const CONTEXT& fault) {
    CONTEXT context = fault;
    std::string stack;
    for (u32 i = 0; i < MaxFrames && context.Rip != 0; i++) {
        const u64 pc = context.Rip;
        DWORD64 image_base = 0;
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        PEXCEPTION_ROUTINE handler = nullptr;
        if (function != nullptr) {
            handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, image_base, pc, function, &context,
                                       &handler_data, &establisher, nullptr);
        } else {
            // A leaf function: the return address is on top of the stack.
            context.Rip = *reinterpret_cast<const u64*>(context.Rsp);
            context.Rsp += sizeof(u64);
        }
        stack += fmt::format(" {}{}", Describe(pc), handler != nullptr ? "*" : "");
    }
    return stack;
}

} // namespace

void ReportFault(void* exception_pointers) {
    const auto* pointers = static_cast<const EXCEPTION_POINTERS*>(exception_pointers);
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    const CONTEXT& c = *pointers->ContextRecord;
    std::string access;
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        const u64 kind = record.ExceptionInformation[0];
        access = fmt::format(", {} of {:#x}",
                             kind == 0   ? "read"
                             : kind == 1 ? "write"
                                         : "execute",
                             record.ExceptionInformation[1]);
    }
    LOG_ERROR(Render_Vulkan,
              "GR2port: exception {:#x}{} at {}; rax {:#x} rcx {:#x} rdx {:#x} rbx {:#x} rsi {:#x} "
              "rdi {:#x} r8 {:#x} r9 {:#x}; stack:{}",
              record.ExceptionCode, access,
              Describe(reinterpret_cast<u64>(record.ExceptionAddress)), c.Rax, c.Rcx, c.Rdx, c.Rbx,
              c.Rsi, c.Rdi, c.R8, c.R9, Stack(c));
}

} // namespace Gr2Port

#endif
