// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>
// windows.h first
#include <shobjidl.h>

#include "launcher/folder_picker.h"

namespace Gr2Port::Launcher {

// The picker is a COM object.
std::optional<std::filesystem::path> PickFolder(HWND owner, const std::wstring& title) {
    std::optional<std::filesystem::path> folder;
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IFileOpenDialog* picker = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&picker)))) {
        FILEOPENDIALOGOPTIONS options = 0;
        picker->GetOptions(&options);
        picker->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        picker->SetTitle(title.c_str());
        IShellItem* item = nullptr;
        if (SUCCEEDED(picker->Show(owner)) && SUCCEEDED(picker->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
        picker->Release();
    }
    if (com) {
        CoUninitialize();
    }
    return folder;
}

} // namespace Gr2Port::Launcher
