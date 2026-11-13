// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>
// windows.h first
#include <commctrl.h>
#include <dxgi.h>

#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/language/language.h"
#include "launcher/driver_notice.h"

#pragma comment(lib, "dxgi.lib")

namespace Gr2Port::Launcher {

namespace {

constexpr const wchar_t* Title = L"Gravity Rush 2";

constexpr UINT NvidiaVendor = 0x10de;

// An NVIDIA driver's version in hundredths: 59597 is 595.97, the oldest driver DLSS needs.
constexpr u32 OldestDriver = 59597;

// The version in hundredths of the NVIDIA driver Windows numbers version: Windows' 32.0.15.9597
// is NVIDIA's 595.97, the last digit of the third number and then the fourth number.
u32 NvidiaVersion(LARGE_INTEGER version) {
    return HIWORD(version.LowPart) % 10 * 10000 + LOWORD(version.LowPart);
}

// The driver version of the first NVIDIA RTX GPU; none for a PC without one, or when Windows
// does not tell the version.
std::optional<u32> RtxDriverVersion() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return std::nullopt;
    }
    std::optional<u32> found;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !found && SUCCEEDED(factory->EnumAdapters1(i, &adapter)); i++) {
        DXGI_ADAPTER_DESC1 desc{};
        LARGE_INTEGER version{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == NvidiaVendor &&
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            std::wstring_view{desc.Description}.find(L"RTX") != std::wstring_view::npos &&
            SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version))) {
            found = NvidiaVersion(version);
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

// A version in hundredths as NVIDIA writes it.
std::wstring Shown(u32 version) {
    const u32 hundredths = version % 100;
    return std::to_wstring(version / 100) + (hundredths < 10 ? L".0" : L".") +
           std::to_wstring(hundredths);
}

} // namespace

void ShowDriverNotice(HWND owner) {
    if (!EmulatorSettings.IsNvidiaDriverNotice()) {
        return;
    }
    const std::optional<u32> version = RtxDriverVersion();
    if (!version || *version >= OldestDriver) {
        return;
    }
    const std::array<std::wstring, 2> versions{Shown(*version), Shown(OldestDriver)};
    const std::wstring heading = Language::Text(L"Update your graphics driver");
    const std::wstring text = Language::Text(
        L"Your NVIDIA graphics driver is version {0}. Update it to version {1} or newer to use "
        L"DLSS.",
        versions);
    const std::wstring box = Language::Text(L"Don't show again");

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = Title;
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = heading.c_str();
    config.pszContent = text.c_str();
    config.pszVerificationText = box.c_str();
    BOOL ticked = FALSE;
    if (SUCCEEDED(TaskDialogIndirect(&config, nullptr, nullptr, &ticked)) && ticked) {
        EmulatorSettings.SetNvidiaDriverNotice(false);
        EmulatorSettings.Save();
    }
}

} // namespace Gr2Port::Launcher
