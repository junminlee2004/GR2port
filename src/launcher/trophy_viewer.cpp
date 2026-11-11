// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <string>
#include <vector>
#include <windows.h>
// windows.h first
#include <cmrc/cmrc.hpp>
#include <commctrl.h>
#include <shlwapi.h>

#include "common/string_util.h"
#include "core/game_check/trophy_list.h"
#include "core/language/language.h"
#include "launcher/art.h"
#include "launcher/trophy_viewer.h"

CMRC_DECLARE(res);

namespace Gr2Port::Launcher {

namespace {

// Pixels at 96 DPI in the dialog's client area.
constexpr int ClientWidth = 640;
constexpr int ClientHeight = 470;
constexpr RECT ProgressLine{10, 10, 630, 25};
constexpr std::array<RECT, 3> FilterBoxes{
    RECT{10, 32, 210, 49},
    RECT{216, 32, 426, 49},
    RECT{432, 32, 630, 49},
};
constexpr RECT ListArea{10, 58, 630, 398};
// The description of the trophy picked, which the list cuts where its column ends.
constexpr RECT DetailLine{10, 404, 630, 432};
constexpr RECT OkButton{557, 438, 630, 460};
// The list's columns: the trophy's picture, name, description, time earned and grade.
constexpr std::array<const wchar_t*, 5> Headers{L"Trophy", L"Name", L"Description",
                                                L"Time Unlocked", L"Type"};
constexpr std::array<int, 5> ColumnWidths{68, 140, 228, 110, 52};
constexpr int TimeColumn = 3;
constexpr int GradeColumn = 4;
constexpr int IconSize = 48;
constexpr int GradeSize = 32;
constexpr std::array<char, 4> Grades{'B', 'S', 'G', 'P'};
constexpr std::array<const char*, Grades.size()> GradeFiles{
    "src/resources/bronze.png", "src/resources/silver.png", "src/resources/gold.png",
    "src/resources/platinum.png"};

// The filters: the earned trophies, the others, and the hidden ones among the others.
constexpr std::array<const wchar_t*, 3> Filters{
    L"Show Earned Trophies", L"Show Not Earned Trophies", L"Show Hidden Trophies"};
constexpr std::array<bool, 3> FilterDefaults{true, true, false};
constexpr size_t EarnedFilter = 0;
constexpr size_t NotEarnedFilter = 1;
constexpr size_t HiddenFilter = 2;

enum Control : int {
    IdProgress = 2101,
    IdList,
    IdDetail,
    IdFilters,
};

struct Viewer {
    std::vector<Trophies::Trophy> trophies;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    HFONT font = nullptr;
    HWND list = nullptr;
    // The grades' pictures, then each trophy's.
    HIMAGELIST pictures = nullptr;
};

// English text in the language in use, with each {N} replaced by values[N].
std::wstring Tr(std::wstring_view english, const std::vector<std::wstring>& values = {}) {
    return Gr2Port::Language::Text(english, values);
}

int Scaled(const Viewer& viewer, int value) {
    return MulDiv(value, static_cast<int>(viewer.dpi), USER_DEFAULT_SCREEN_DPI);
}

HWND MakeControl(const Viewer& viewer, HWND parent, const wchar_t* type, const wchar_t* text,
                 DWORD style, int id, const RECT& area) {
    const HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style, Scaled(viewer, area.left),
        Scaled(viewer, area.top), Scaled(viewer, area.right - area.left),
        Scaled(viewer, area.bottom - area.top), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(viewer.font), FALSE);
    return control;
}

// Adds a picture to the list's, drawn inner pixels across in the middle of a white square. A
// picture that does not load leaves the square white.
void AddPicture(const Viewer& viewer, Gdiplus::Image* picture, int inner) {
    const int side = Scaled(viewer, IconSize);
    const float size = static_cast<float>(Scaled(viewer, inner));
    const float edge = (static_cast<float>(side) - size) / 2.0f;
    const HBITMAP bitmap = Art::Render(side, side, [&](Gdiplus::Graphics& graphics) {
        graphics.Clear(Gdiplus::Color{255, 255, 255, 255});
        if (picture != nullptr && picture->GetLastStatus() == Gdiplus::Ok) {
            graphics.DrawImage(picture, Gdiplus::RectF{edge, edge, size, size});
        }
    });
    if (bitmap == nullptr) {
        return;
    }
    ImageList_Add(viewer.pictures, bitmap, nullptr);
    DeleteObject(bitmap);
}

// The list's pictures: the built-in ones of the four grades, then each trophy's own.
void MakePictures(Viewer& viewer) {
    const int side = Scaled(viewer, IconSize);
    viewer.pictures = ImageList_Create(side, side, ILC_COLOR32,
                                       static_cast<int>(Grades.size() + viewer.trophies.size()), 0);
    const cmrc::embedded_filesystem files = cmrc::res::get_filesystem();
    for (const char* const name : GradeFiles) {
        const cmrc::file file = files.open(name);
        IStream* const stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(file.begin()),
                                                  static_cast<UINT>(file.size()));
        // GDI+ reads the stream while it draws, so the stream outlives the picture.
        {
            Gdiplus::Bitmap picture{stream};
            AddPicture(viewer, stream != nullptr ? &picture : nullptr, GradeSize);
        }
        if (stream != nullptr) {
            stream->Release();
        }
    }
    for (const Trophies::Trophy& trophy : viewer.trophies) {
        Gdiplus::Bitmap picture{trophy.icon.c_str()};
        AddPicture(viewer, &picture, IconSize);
    }
    SendMessageW(viewer.list, LVM_SETIMAGELIST, LVSIL_SMALL,
                 reinterpret_cast<LPARAM>(viewer.pictures));
}

// The time a trophy was earned, in the player's short date and time formats.
std::wstring TimeText(u64 seconds) {
    // A FILETIME counts 100 ns from 1601.
    const ULONGLONG ticks = (seconds + 11644473600ULL) * 10000000ULL;
    const FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME universal{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&utc, &universal) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local)) {
        return {};
    }
    std::array<wchar_t, 64> date{};
    std::array<wchar_t, 64> time{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date.data(),
                    static_cast<int>(date.size()), nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time.data(),
                    static_cast<int>(time.size()));
    return std::wstring{date.data()} + L" " + time.data();
}

// A description's line breaks are spaces in the list.
std::wstring Detail(const Trophies::Trophy& trophy) {
    std::wstring detail = Common::UTF8ToUTF16W(trophy.detail);
    for (wchar_t& c : detail) {
        if (c == L'\n' || c == L'\r') {
            c = L' ';
        }
    }
    return detail;
}

void SetCell(HWND list, int row, int column, const std::wstring& text) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
}

bool FilterOn(HWND window, size_t filter) {
    return IsDlgButtonChecked(window, IdFilters + static_cast<int>(filter)) == BST_CHECKED;
}

// Lists the trophies that pass the filters: a hidden trophy the player has not earned needs the
// hidden filter as well. A row keeps its trophy's index.
void FillList(HWND window, const Viewer& viewer) {
    SendMessageW(viewer.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(viewer.list, LVM_DELETEALLITEMS, 0, 0);
    int row = 0;
    for (size_t i = 0; i < viewer.trophies.size(); ++i) {
        const Trophies::Trophy& trophy = viewer.trophies[i];
        if (!FilterOn(window, trophy.earned ? EarnedFilter : NotEarnedFilter) ||
            (!trophy.earned && trophy.hidden && !FilterOn(window, HiddenFilter))) {
            continue;
        }
        LVITEMW item{};
        item.mask = LVIF_IMAGE | LVIF_PARAM;
        item.iItem = row;
        item.iImage = static_cast<int>(Grades.size() + i);
        item.lParam = static_cast<LPARAM>(i);
        SendMessageW(viewer.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        SetCell(viewer.list, row, 1, Common::UTF8ToUTF16W(trophy.name));
        SetCell(viewer.list, row, 2, Detail(trophy));
        if (trophy.earned && trophy.earned_at != 0) {
            SetCell(viewer.list, row, TimeColumn, TimeText(trophy.earned_at));
        }
        LVITEMW grade{};
        grade.mask = LVIF_IMAGE;
        grade.iItem = row;
        grade.iSubItem = GradeColumn;
        for (size_t g = 0; g < Grades.size(); ++g) {
            if (Grades[g] == trophy.grade) {
                grade.iImage = static_cast<int>(g);
            }
        }
        SendMessageW(viewer.list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&grade));
        ++row;
    }
    SendMessageW(viewer.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(viewer.list, nullptr, TRUE);
    SetDlgItemTextW(window, IdDetail, L"");
}

// Shows the description of the trophy picked under the list.
void ShowDetail(HWND window, const Viewer& viewer) {
    const int row = static_cast<int>(
        SendMessageW(viewer.list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    std::wstring detail;
    if (row >= 0) {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        SendMessageW(viewer.list, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
        const size_t index = static_cast<size_t>(item.lParam);
        if (index < viewer.trophies.size()) {
            detail = Detail(viewer.trophies[index]);
        }
    }
    SetDlgItemTextW(window, IdDetail, detail.c_str());
}

void Build(HWND window, Viewer& viewer) {
    SetWindowTextW(window, Tr(L"Trophies").c_str());
    viewer.dpi = GetDpiForWindow(window);
    viewer.font =
        CreateFontW(-MulDiv(8, static_cast<int>(viewer.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                    FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
    RECT frame{0, 0, Scaled(viewer, ClientWidth), Scaled(viewer, ClientHeight)};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)), viewer.dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT owner{};
    GetWindowRect(GetWindow(window, GW_OWNER), &owner);
    SetWindowPos(window, nullptr, (owner.left + owner.right - width) / 2,
                 (owner.top + owner.bottom - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    size_t earned = 0;
    for (const Trophies::Trophy& trophy : viewer.trophies) {
        earned += trophy.earned ? 1 : 0;
    }
    const size_t total = viewer.trophies.size();
    const std::wstring progress =
        total == 0
            ? Tr(L"No trophies found.")
            : Tr(L"Progress: {0}% ({1}/{2})", {std::to_wstring(earned * 100 / total),
                                               std::to_wstring(earned), std::to_wstring(total)});
    MakeControl(viewer, window, WC_STATICW, progress.c_str(), SS_LEFT, IdProgress, ProgressLine);
    for (size_t i = 0; i < Filters.size(); ++i) {
        const int id = IdFilters + static_cast<int>(i);
        MakeControl(viewer, window, WC_BUTTONW, Tr(Filters[i]).c_str(),
                    BS_AUTOCHECKBOX | WS_TABSTOP, id, FilterBoxes[i]);
        CheckDlgButton(window, id, FilterDefaults[i] ? BST_CHECKED : BST_UNCHECKED);
    }

    viewer.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                      LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                  Scaled(viewer, ListArea.left), Scaled(viewer, ListArea.top),
                                  Scaled(viewer, ListArea.right - ListArea.left),
                                  Scaled(viewer, ListArea.bottom - ListArea.top), window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdList)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(viewer.list, WM_SETFONT, reinterpret_cast<WPARAM>(viewer.font), FALSE);
    SendMessageW(viewer.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_SUBITEMIMAGES | LVS_EX_DOUBLEBUFFER);
    for (size_t i = 0; i < Headers.size(); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.cx = Scaled(viewer, ColumnWidths[i]);
        std::wstring header = Tr(Headers[i]);
        column.pszText = header.data();
        SendMessageW(viewer.list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column));
    }
    MakePictures(viewer);
    MakeControl(viewer, window, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, IdDetail, DetailLine);
    MakeControl(viewer, window, WC_BUTTONW, Tr(L"OK").c_str(), BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK,
                OkButton);
    FillList(window, viewer);
    SetFocus(viewer.list);
}

INT_PTR CALLBACK ViewerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        Build(window, *reinterpret_cast<Viewer*>(lparam));
        return FALSE;
    }
    auto* const viewer = reinterpret_cast<Viewer*>(GetWindowLongPtrW(window, DWLP_USER));
    if (viewer == nullptr) {
        return FALSE;
    }
    switch (message) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IDOK || id == IDCANCEL) {
            EndDialog(window, id);
        } else if (id >= IdFilters && id < IdFilters + static_cast<int>(Filters.size()) &&
                   HIWORD(wparam) == BN_CLICKED) {
            FillList(window, *viewer);
        } else {
            return FALSE;
        }
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->hwndFrom == viewer->list && header->code == LVN_ITEMCHANGED) {
            ShowDetail(window, *viewer);
        }
        return TRUE;
    }
    default:
        return FALSE;
    }
}

} // namespace

void ShowTrophies(HWND owner) {
    struct alignas(4) Template {
        DLGTEMPLATE header;
        WORD menu;
        WORD window_class;
        wchar_t title[16];
    };
    Template dialog_template{};
    dialog_template.header.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    wcscpy_s(dialog_template.title, L"Trophies");

    Viewer viewer;
    for (Trophies::List& list : Trophies::Read(Language::Current())) {
        for (Trophies::Trophy& trophy : list.trophies) {
            viewer.trophies.push_back(std::move(trophy));
        }
    }
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog_template.header, owner, &ViewerProc,
                            reinterpret_cast<LPARAM>(&viewer));
    if (viewer.pictures != nullptr) {
        ImageList_Destroy(viewer.pictures);
    }
    DeleteObject(viewer.font);
}

} // namespace Gr2Port::Launcher
