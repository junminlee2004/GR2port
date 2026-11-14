// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "common/logging/log.h"
#include "graphics/gr2port/overlays.h"

namespace Gr2Port {

namespace {

// The variables that keep RTSS's and MangoHud's implicit Vulkan layers out of an instance, as
// their layer manifests name them.
constexpr std::array<const char*, 2> LayerSwitches{"DISABLE_RTSS_LAYER", "DISABLE_MANGOHUD"};

// The Vulkan loader reads the process environment itself, so on Windows these go through the
// Win32 block rather than the C runtime's copy.
std::optional<std::string> ReadVariable(const char* name) {
#ifdef _WIN32
    char value[256];
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) {
        return std::nullopt;
    }
    return std::string(value, length);
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
#endif
}

void WriteVariable(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value ? value->c_str() : nullptr);
#else
    if (value) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

#ifdef _WIN32

// RTSS's hook opens every display adapter by its device name about once a second, inside the
// present, to read the process's video memory use. In a PC with a second GPU that open can take
// Windows up to 0.7 s, and the present waits for it. The open is the system call
// NtGdiDdDDIOpenAdapterFromDeviceName in win32u.dll, which gdi32.dll's D3DKMT export forwards
// to. The call's first bytes jump here; a copy of the call runs it for every caller but RTSS
// opening an adapter the renderer does not draw on, which gets StatusNoSuchDevice.
constexpr wchar_t OverlayModule[] = L"RTSSHooks64.dll";
constexpr LONG StatusNoSuchDevice = static_cast<LONG>(0xc000000e);
// The system call's code on x64: mov r10, rcx; mov eax, number; test byte [0x7ffe0308], 1;
// jne +3; syscall; ret; int 0x2e; ret.
constexpr std::size_t CallBytes = 24;
constexpr std::array<u8, 4> CallHead{0x4c, 0x8b, 0xd1, 0xb8};
constexpr std::array<u8, 16> CallTail{0xf6, 0x04, 0x25, 0x08, 0x03, 0xfe, 0x7f, 0x01,
                                      0x75, 0x03, 0x0f, 0x05, 0xc3, 0xcd, 0x2e, 0xc3};
// mov rax, address; jmp rax.
constexpr std::size_t JumpBytes = 12;

// D3DKMT_OPENADAPTERFROMDEVICENAME.
struct AdapterOpen {
    const wchar_t* device_name;
    u32 adapter;
    LUID luid;
};
using OpenCall = LONG(__stdcall*)(AdapterOpen*);

OpenCall g_open = nullptr;
u32 g_vendor = 0;
u32 g_device = 0;

// The PCI vendor and device in a display adapter's device name, as in
// \\?\pci#ven_10de&dev_1e07&subsys_...#...#{...}.
bool IdsOf(const wchar_t* name, u32& vendor, u32& device) {
    const auto number = [name](const wchar_t* key, u32& value) {
        for (const wchar_t* p = name; *p != 0; p++) {
            if (_wcsnicmp(p, key, 4) == 0) {
                wchar_t* end = nullptr;
                value = wcstoul(p + 4, &end, 16);
                return end == p + 8;
            }
        }
        return false;
    };
    return number(L"ven_", vendor) && number(L"dev_", device);
}

bool FromOverlay(const void* caller) {
    HMODULE module = nullptr;
    const DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    return GetModuleHandleExW(flags, static_cast<LPCWSTR>(caller), &module) != 0 &&
           module == GetModuleHandleW(OverlayModule);
}

LONG __stdcall OpenAdapter(AdapterOpen* open) {
    const void* const caller = __builtin_return_address(0);
    u32 vendor = 0;
    u32 device = 0;
    if (open != nullptr && open->device_name != nullptr && FromOverlay(caller) &&
        IdsOf(open->device_name, vendor, device) && (vendor != g_vendor || device != g_device)) {
        return StatusNoSuchDevice;
    }
    return g_open(open);
}

#endif

} // namespace

#ifdef _WIN32
void KeepOverlayOffOtherAdapters(u32 vendor, u32 device) {
    if (g_open != nullptr || GetModuleHandleW(OverlayModule) == nullptr) {
        return;
    }
    const HMODULE win32u = GetModuleHandleW(L"win32u.dll");
    u8* const call =
        win32u != nullptr
            ? reinterpret_cast<u8*>(GetProcAddress(win32u, "NtGdiDdDDIOpenAdapterFromDeviceName"))
            : nullptr;
    if (call == nullptr || !std::equal(CallHead.begin(), CallHead.end(), call) ||
        !std::equal(CallTail.begin(), CallTail.end(), call + 8)) {
        LOG_WARNING(Render_Vulkan,
                    "GR2port: the adapter open call is not the expected code; RTSS's opens of "
                    "other display adapters reach Windows");
        return;
    }
    u8* const copy = static_cast<u8*>(
        VirtualAlloc(nullptr, CallBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    DWORD old_protection = 0;
    if (copy == nullptr ||
        VirtualProtect(call, JumpBytes, PAGE_EXECUTE_READWRITE, &old_protection) == 0) {
        return;
    }
    std::memcpy(copy, call, CallBytes);
    g_vendor = vendor;
    g_device = device;
    g_open = reinterpret_cast<OpenCall>(copy);
    std::array<u8, JumpBytes> jump{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    const u64 target = reinterpret_cast<u64>(&OpenAdapter);
    std::memcpy(jump.data() + 2, &target, sizeof(target));
    std::memcpy(call, jump.data(), JumpBytes);
    VirtualProtect(call, JumpBytes, old_protection, &old_protection);
    FlushInstructionCache(GetCurrentProcess(), call, JumpBytes);
}
#else
void KeepOverlayOffOtherAdapters(u32, u32) {}
#endif

OverlaysOutOfInstance::OverlaysOutOfInstance() {
    for (std::size_t i = 0; i < LayerSwitches.size(); i++) {
        saved[i] = ReadVariable(LayerSwitches[i]);
        WriteVariable(LayerSwitches[i], "1");
    }
}

OverlaysOutOfInstance::~OverlaysOutOfInstance() {
    for (std::size_t i = 0; i < LayerSwitches.size(); i++) {
        WriteVariable(LayerSwitches[i], saved[i]);
    }
}

void LogOverlays() {
#ifdef _WIN32
    LOG_INFO(Render_Vulkan, "GR2port: RTSS hooks in the process: {}",
             GetModuleHandleW(L"RTSSHooks64.dll") != nullptr);
#else
    void* const mangohud = dlopen("libMangoHud.so", RTLD_NOW | RTLD_NOLOAD);
    LOG_INFO(Render_Vulkan, "GR2port: MangoHud in the process: {}", mangohud != nullptr);
    if (mangohud != nullptr) {
        dlclose(mangohud);
    }
#endif
}

} // namespace Gr2Port
