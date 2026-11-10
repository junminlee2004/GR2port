// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <optional>
#include <set>
#include <string_view>
#include <windows.h>
// windows.h first
#include <commctrl.h>

#include "core/language/language.h"
#include "input/game_functions.h"
#include "input/input_handler.h"
#include "launcher/key_bindings.h"

namespace Gr2Port::Launcher {

namespace {

namespace Game = Input::GameFunctions;

constexpr const wchar_t* CaptureClass = L"GR2portKeyCapture";

// Pixels at 96 DPI in the dialog's client area.
constexpr int ClientWidth = 560;
constexpr int ClientHeight = 430;
constexpr RECT TabArea{10, 10, 550, 330};
constexpr RECT TypeLine{10, 338, 550, 353};
constexpr RECT StatusLine{10, 358, 550, 373};
constexpr RECT ChangeButton{10, 398, 110, 420};
constexpr RECT ClearButton{116, 398, 206, 420};
constexpr RECT DefaultsButton{212, 398, 302, 420};
constexpr RECT OkButton{394, 398, 467, 420};
constexpr RECT CancelButton{475, 398, 548, 420};
constexpr std::array<int, 3> ColumnWidths{180, 185, 140};
constexpr int CaptureWidth = 320;
constexpr int CaptureHeight = 90;
constexpr int CaptureSeconds = 5;

enum Control : int {
    IdTypes = 2001,
    IdList,
    IdType,
    IdStatus,
    IdChange,
    IdClear,
    IdDefaults,
};

constexpr std::array<const wchar_t*, Game::TypeCount> TypeNames{L"Type A", L"Type B", L"Type C",
                                                                L"Type D", L"Type E"};

struct Editor {
    KeyBindings keys;
    u32 type = 0;
    u32 game_type = 0;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    HFONT font = nullptr;
    HWND tab = nullptr;
    HWND list = nullptr;
};

// English text in the language in use, with each {N} replaced by values[N].
std::wstring Tr(std::wstring_view english, const std::vector<std::wstring>& values = {}) {
    return Gr2Port::Language::Text(english, values);
}

std::wstring Wide(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

int Scaled(const Editor& editor, int value) {
    return MulDiv(value, static_cast<int>(editor.dpi), USER_DEFAULT_SCREEN_DPI);
}

// A config line without its comment and spaces, as the input config parser reads it.
std::string Compact(std::string_view line) {
    std::string compact(line.substr(0, line.find('#')));
    std::erase_if(compact, [](unsigned char c) { return std::isspace(c) != 0; });
    return compact;
}

// --------------------------------------------------------------------------------- key names

// The input config's names of the keys and buttons whose text is not the name capitalised.
constexpr std::pair<std::string_view, std::wstring_view> KeyTexts[]{
    {"space", L"Space"},
    {"escape", L"Esc"},
    {"enter", L"Enter"},
    {"backspace", L"Backspace"},
    {"tab", L"Tab"},
    {"capslock", L"Caps Lock"},
    {"lshift", L"Left Shift"},
    {"rshift", L"Right Shift"},
    {"lctrl", L"Left Ctrl"},
    {"rctrl", L"Right Ctrl"},
    {"lalt", L"Left Alt"},
    {"ralt", L"Right Alt"},
    {"lwin", L"Left Windows"},
    {"rwin", L"Right Windows"},
    {"up", L"Up Arrow"},
    {"down", L"Down Arrow"},
    {"left", L"Left Arrow"},
    {"right", L"Right Arrow"},
    {"pgup", L"Page Up"},
    {"pgdown", L"Page Down"},
    {"printscreen", L"Print Screen"},
    {"scrolllock", L"Scroll Lock"},
    {"pausebreak", L"Pause"},
    {"grave", L"`"},
    {"minus", L"-"},
    {"equals", L"="},
    {"lbracket", L"["},
    {"rbracket", L"]"},
    {"backslash", L"\\"},
    {"semicolon", L";"},
    {"apostrophe", L"'"},
    {"comma", L","},
    {"period", L"."},
    {"slash", L"/"},
    {"kpperiod", L"Num ."},
    {"kpslash", L"Num /"},
    {"kpasterisk", L"Num *"},
    {"kpminus", L"Num -"},
    {"kpplus", L"Num +"},
    {"kpenter", L"Num Enter"},
    {"leftbutton", L"Left Mouse Button"},
    {"rightbutton", L"Right Mouse Button"},
    {"middlebutton", L"Middle Mouse Button"},
    {"sidebuttonback", L"Mouse Back"},
    {"sidebuttonforward", L"Mouse Forward"},
    {"mousewheelup", L"Mouse Wheel Up"},
    {"mousewheeldown", L"Mouse Wheel Down"},
    {"mousewheelleft", L"Mouse Wheel Left"},
    {"mousewheelright", L"Mouse Wheel Right"},
};

std::wstring KeyText(std::string_view name) {
    if (name.empty()) {
        return Tr(L"None");
    }
    for (const auto& [key, text] : KeyTexts) {
        if (key == name) {
            return Tr(text);
        }
    }
    if (name.size() == 3 && name.starts_with("kp") && std::isdigit(name[2])) {
        return Tr(L"Num {0}", {Wide(name.substr(2))});
    }
    std::wstring text = Wide(name);
    if (name.size() == 1 || (name[0] == 'f' && name.size() <= 3)) {
        std::ranges::transform(text, text.begin(), [](wchar_t c) { return std::towupper(c); });
    } else {
        text[0] = std::towupper(text[0]);
    }
    return text;
}

// The config name of a key a WM_KEYDOWN reports, or empty for one the config cannot name.
std::string KeyName(WPARAM key, LPARAM flags) {
    const bool extended = (flags & (1 << 24)) != 0;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        return std::string(1, static_cast<char>(std::tolower(static_cast<int>(key))));
    }
    if (key >= VK_F1 && key <= VK_F12) {
        return "f" + std::to_string(key - VK_F1 + 1);
    }
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) {
        return "kp" + std::to_string(key - VK_NUMPAD0);
    }
    switch (key) {
    case VK_SHIFT: {
        const UINT scan = (flags >> 16) & 0xff;
        return MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? "rshift" : "lshift";
    }
    case VK_CONTROL:
        return extended ? "rctrl" : "lctrl";
    case VK_MENU:
        return extended ? "ralt" : "lalt";
    case VK_RETURN:
        return extended ? "kpenter" : "enter";
    case VK_SPACE:
        return "space";
    case VK_ESCAPE:
        return "escape";
    case VK_TAB:
        return "tab";
    case VK_BACK:
        return "backspace";
    case VK_CAPITAL:
        return "capslock";
    case VK_SNAPSHOT:
        return "printscreen";
    case VK_SCROLL:
        return "scrolllock";
    case VK_PAUSE:
        return "pausebreak";
    case VK_INSERT:
        return "insert";
    case VK_DELETE:
        return "delete";
    case VK_HOME:
        return "home";
    case VK_END:
        return "end";
    case VK_PRIOR:
        return "pgup";
    case VK_NEXT:
        return "pgdown";
    case VK_UP:
        return "up";
    case VK_DOWN:
        return "down";
    case VK_LEFT:
        return "left";
    case VK_RIGHT:
        return "right";
    case VK_LWIN:
        return "lwin";
    case VK_RWIN:
        return "rwin";
    case VK_OEM_3:
        return "grave";
    case VK_OEM_MINUS:
        return "minus";
    case VK_OEM_PLUS:
        return "equals";
    case VK_OEM_4:
        return "lbracket";
    case VK_OEM_6:
        return "rbracket";
    case VK_OEM_5:
        return "backslash";
    case VK_OEM_1:
        return "semicolon";
    case VK_OEM_7:
        return "apostrophe";
    case VK_OEM_COMMA:
        return "comma";
    case VK_OEM_PERIOD:
        return "period";
    case VK_OEM_2:
        return "slash";
    case VK_DECIMAL:
        return "kpperiod";
    case VK_DIVIDE:
        return "kpslash";
    case VK_MULTIPLY:
        return "kpasterisk";
    case VK_SUBTRACT:
        return "kpminus";
    case VK_ADD:
        return "kpplus";
    default:
        return {};
    }
}

// ------------------------------------------------------------------------------- key capture

struct Capture {
    std::wstring function;
    HFONT font = nullptr;
    int seconds_left = CaptureSeconds;
    bool done = false;
    std::string key;
};

void Finish(Capture& capture, std::string key) {
    if (!capture.done && (key.empty() || Input::string_to_keyboard_key_map.contains(key))) {
        capture.key = std::move(key);
        capture.done = true;
    }
}

// A popup that takes the next key or mouse button, or nothing when the countdown ends.
LRESULT CALLBACK CaptureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* const capture = reinterpret_cast<Capture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (capture == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam != VK_SNAPSHOT) {
            Finish(*capture, KeyName(wparam, lparam));
        }
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        // Print Screen reports only its release.
        if (wparam == VK_SNAPSHOT) {
            Finish(*capture, "printscreen");
        }
        return 0;
    case WM_LBUTTONDOWN:
        Finish(*capture, "leftbutton");
        return 0;
    case WM_RBUTTONDOWN:
        Finish(*capture, "rightbutton");
        return 0;
    case WM_MBUTTONDOWN:
        Finish(*capture, "middlebutton");
        return 0;
    case WM_XBUTTONDOWN:
        Finish(*capture,
               GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? "sidebuttonback" : "sidebuttonforward");
        return TRUE;
    case WM_MOUSEWHEEL:
        Finish(*capture, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? "mousewheelup" : "mousewheeldown");
        return 0;
    case WM_MOUSEHWHEEL:
        Finish(*capture, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? "mousewheelright" : "mousewheelleft");
        return 0;
    case WM_TIMER:
        if (--capture->seconds_left <= 0) {
            capture->done = true;
        }
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_CAPTURECHANGED:
        capture->done = true;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT area{};
        GetClientRect(window, &area);
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const HGDIOBJ old_font = SelectObject(dc, capture->font);
        SetBkMode(dc, TRANSPARENT);
        const std::wstring text =
            Tr(L"Press a key or mouse button for\n{0}\n\nNothing changes in {1} s.",
               {capture->function, std::to_wstring(capture->seconds_left)});
        InflateRect(&area, 0, -MulDiv(area.bottom, 1, 10));
        DrawTextW(dc, text.c_str(), -1, &area, DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, old_font);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

// Waits for a key or mouse button over owner. Empty when the countdown ends first.
std::string CaptureKey(HWND owner, const Editor& editor, const std::wstring& function) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW capture_class{};
    capture_class.cbSize = sizeof(capture_class);
    capture_class.lpfnWndProc = &CaptureProc;
    capture_class.hInstance = instance;
    capture_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    capture_class.lpszClassName = CaptureClass;
    RegisterClassExW(&capture_class);

    RECT owner_area{};
    GetWindowRect(owner, &owner_area);
    const int width = Scaled(editor, CaptureWidth);
    const int height = Scaled(editor, CaptureHeight);
    const HWND window =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CaptureClass, L"", WS_POPUP | WS_BORDER,
                        (owner_area.left + owner_area.right - width) / 2,
                        (owner_area.top + owner_area.bottom - height) / 2, width, height, owner,
                        nullptr, instance, nullptr);
    if (window == nullptr) {
        return {};
    }
    Capture capture{function, editor.font};
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&capture));
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    SetCapture(window);
    SetTimer(window, 1, 1000, nullptr);
    MSG message{};
    while (!capture.done) {
        const BOOL got = GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        DispatchMessageW(&message);
    }
    KillTimer(window, 1);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    ReleaseCapture();
    EnableWindow(owner, TRUE);
    DestroyWindow(window);
    SetActiveWindow(owner);
    return capture.key;
}

// ------------------------------------------------------------------------------------ dialog

HWND MakeControl(const Editor& editor, HWND parent, const wchar_t* type, const wchar_t* text,
                 DWORD style, int id, const RECT& area) {
    const HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style, Scaled(editor, area.left),
        Scaled(editor, area.top), Scaled(editor, area.right - area.left),
        Scaled(editor, area.bottom - area.top), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(editor.font), FALSE);
    return control;
}

void SetCell(HWND list, int row, int column, const std::wstring& text) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
}

// The pad column shows the type's pad inputs; the key column is the same in every type.
void ShowRows(const Editor& editor) {
    const auto functions = Game::Functions();
    for (size_t i = 0; i < functions.size(); ++i) {
        SetCell(editor.list, static_cast<int>(i), 1, Tr(Wide(functions[i].pad[editor.type])));
        SetCell(editor.list, static_cast<int>(i), 2, KeyText(editor.keys[i]));
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT;
    std::wstring header = Tr(L"Controller ({0})", {Tr(TypeNames[editor.type])});
    column.pszText = header.data();
    SendMessageW(editor.list, LVM_SETCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
}

void ShowStatus(HWND window, const std::wstring& text) {
    SetDlgItemTextW(window, IdStatus, text.c_str());
}

int SelectedRow(const Editor& editor) {
    return static_cast<int>(
        SendMessageW(editor.list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

// Takes a key for the selected function. A key that another function had moves to this one.
void ChangeKey(HWND window, Editor& editor) {
    const int row = SelectedRow(editor);
    const auto functions = Game::Functions();
    if (row < 0 || row >= static_cast<int>(functions.size())) {
        ShowStatus(window, Tr(L"Pick a game function first."));
        return;
    }
    const std::wstring label = Tr(Wide(functions[row].label));
    const std::string key = CaptureKey(window, editor, label);
    if (key.empty()) {
        ShowStatus(window, Tr(L"No key was pressed; {0} keeps its key.", {label}));
        return;
    }
    std::wstring status = Tr(L"{0} now does {1}.", {KeyText(key), label});
    for (size_t i = 0; i < editor.keys.size(); ++i) {
        if (static_cast<int>(i) != row && editor.keys[i] == key) {
            editor.keys[i].clear();
            status += L" " + Tr(L"{0} has no key now.", {Tr(Wide(functions[i].label))});
        }
    }
    editor.keys[row] = key;
    ShowRows(editor);
    ShowStatus(window, status);
}

void Build(HWND window, Editor& editor) {
    SetWindowTextW(window, Tr(L"Key Bindings").c_str());
    editor.dpi = GetDpiForWindow(window);
    editor.font =
        CreateFontW(-MulDiv(8, static_cast<int>(editor.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                    FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
    RECT frame{0, 0, Scaled(editor, ClientWidth), Scaled(editor, ClientHeight)};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)), editor.dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT owner{};
    GetWindowRect(GetWindow(window, GW_OWNER), &owner);
    SetWindowPos(window, nullptr, (owner.left + owner.right - width) / 2,
                 (owner.top + owner.bottom - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    editor.tab = MakeControl(editor, window, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS,
                             IdTypes, TabArea);
    for (size_t i = 0; i < TypeNames.size(); ++i) {
        std::wstring name = Tr(TypeNames[i]);
        if (i == editor.game_type) {
            name = Tr(L"{0} (in use)", {name});
        }
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = name.data();
        SendMessageW(editor.tab, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&item));
    }
    SendMessageW(editor.tab, TCM_SETCURSEL, editor.type, 0);
    RECT list_area{0, 0, Scaled(editor, TabArea.right - TabArea.left),
                   Scaled(editor, TabArea.bottom - TabArea.top)};
    TabCtrl_AdjustRect(editor.tab, FALSE, &list_area);
    OffsetRect(&list_area, Scaled(editor, TabArea.left), Scaled(editor, TabArea.top));
    editor.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                      LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                  list_area.left, list_area.top, list_area.right - list_area.left,
                                  list_area.bottom - list_area.top, window,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdList)),
                                  GetModuleHandleW(nullptr), nullptr);
    SendMessageW(editor.list, WM_SETFONT, reinterpret_cast<WPARAM>(editor.font), FALSE);
    SendMessageW(editor.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SetWindowPos(editor.list, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    constexpr std::array<const wchar_t*, 3> Headers{L"Game Function", L"Controller", L"Key"};
    for (size_t i = 0; i < Headers.size(); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.cx = Scaled(editor, ColumnWidths[i]);
        std::wstring header = Tr(Headers[i]);
        column.pszText = header.data();
        SendMessageW(editor.list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column));
    }
    const auto functions = Game::Functions();
    for (size_t i = 0; i < functions.size(); ++i) {
        const std::wstring label = Tr(Wide(functions[i].label));
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(label.c_str());
        SendMessageW(editor.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }
    ShowRows(editor);

    const std::wstring in_use = Tr(L"The game used {0} last (Options > Controls). Each key does "
                                   L"its function in every type.",
                                   {Tr(TypeNames[editor.game_type])});
    MakeControl(editor, window, WC_STATICW, in_use.c_str(), SS_LEFT, IdType, TypeLine);
    MakeControl(editor, window, WC_STATICW,
                Tr(L"Double-click a function, then press its key. Mouse buttons and the wheel "
                   L"count.")
                    .c_str(),
                SS_LEFT, IdStatus, StatusLine);
    MakeControl(editor, window, WC_BUTTONW, Tr(L"Change Key...").c_str(),
                BS_PUSHBUTTON | WS_TABSTOP, IdChange, ChangeButton);
    MakeControl(editor, window, WC_BUTTONW, Tr(L"Clear Key").c_str(), BS_PUSHBUTTON | WS_TABSTOP,
                IdClear, ClearButton);
    MakeControl(editor, window, WC_BUTTONW, Tr(L"Defaults").c_str(), BS_PUSHBUTTON | WS_TABSTOP,
                IdDefaults, DefaultsButton);
    MakeControl(editor, window, WC_BUTTONW, Tr(L"OK").c_str(), BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK,
                OkButton);
    MakeControl(editor, window, WC_BUTTONW, Tr(L"Cancel").c_str(), BS_PUSHBUTTON | WS_TABSTOP,
                IDCANCEL, CancelButton);
    SetFocus(editor.list);
}

INT_PTR CALLBACK EditorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        Build(window, *reinterpret_cast<Editor*>(lparam));
        return FALSE;
    }
    auto* const editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(window, DWLP_USER));
    if (editor == nullptr) {
        return FALSE;
    }
    switch (message) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IdChange:
            ChangeKey(window, *editor);
            return TRUE;
        case IdClear:
            if (const int row = SelectedRow(*editor); row >= 0) {
                editor->keys[row].clear();
                ShowRows(*editor);
            }
            return TRUE;
        case IdDefaults:
            editor->keys = DefaultKeyBindings();
            ShowRows(*editor);
            ShowStatus(window, Tr(L"Every function has its default key again."));
            return TRUE;
        case IDOK:
        case IDCANCEL:
            EndDialog(window, LOWORD(wparam));
            return TRUE;
        default:
            return FALSE;
        }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->hwndFrom == editor->tab && header->code == TCN_SELCHANGE) {
            editor->type = static_cast<u32>(SendMessageW(editor->tab, TCM_GETCURSEL, 0, 0));
            ShowRows(*editor);
        } else if (header->hwndFrom == editor->list && header->code == NM_DBLCLK) {
            ChangeKey(window, *editor);
        }
        return TRUE;
    }
    default:
        return FALSE;
    }
}

} // namespace

KeyBindings DefaultKeyBindings() {
    KeyBindings keys;
    for (const Game::Function& function : Game::Functions()) {
        keys.emplace_back(function.key);
    }
    return keys;
}

KeyBindings ReadKeyBindings(const std::filesystem::path& file) {
    const auto functions = Game::Functions();
    KeyBindings keys = DefaultKeyBindings();
    std::vector<bool> read(functions.size());
    bool any = false;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        const auto equal = compact.find('=');
        const Game::Function* const function =
            equal == std::string::npos ? nullptr : Game::Find(compact.substr(0, equal));
        if (function == nullptr) {
            continue;
        }
        if (!any) {
            any = true;
            keys.assign(functions.size(), {});
        }
        const size_t index = function - functions.data();
        if (!read[index]) {
            read[index] = true;
            const std::string key = compact.substr(equal + 1);
            keys[index] = key == "unmapped" ? std::string{} : key;
        }
    }
    return keys;
}

bool SaveKeyBindings(const std::filesystem::path& file, const KeyBindings& keys) {
    const auto functions = Game::Functions();
    // The pad inputs the functions bind, and R2 and L3, which earlier configs gave keys.
    std::set<std::string_view> pad_outputs{"r2", "l3"};
    for (const Game::Function& function : functions) {
        if (!function.output.empty()) {
            pad_outputs.insert(function.output);
        }
    }
    const auto keyboard_only = [](std::string_view inputs) {
        while (!inputs.empty()) {
            const auto comma = inputs.find(',');
            if (!Input::string_to_keyboard_key_map.contains(std::string(inputs.substr(0, comma)))) {
                return false;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            inputs.remove_prefix(comma + 1);
        }
        return true;
    };
    const auto blank = [](std::string_view line) {
        return std::ranges::all_of(line, [](unsigned char c) { return std::isspace(c) != 0; });
    };
    // The removed lines leave no runs of blank lines.
    std::vector<std::string> lines;
    std::optional<size_t> block_at;
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            const std::string compact = Compact(line);
            const auto equal = compact.find('=');
            if (equal != std::string::npos) {
                const std::string output = compact.substr(0, equal);
                if (Game::Find(output) != nullptr ||
                    (pad_outputs.contains(output) && keyboard_only(compact.substr(equal + 1)))) {
                    block_at = block_at.value_or(lines.size());
                    continue;
                }
            }
            if (blank(line) && !lines.empty() && blank(lines.back())) {
                continue;
            }
            lines.push_back(std::move(line));
        }
    }
    std::vector<std::string> block;
    for (size_t i = 0; i < functions.size(); ++i) {
        const std::string& key = i < keys.size() ? keys[i] : std::string{};
        block.push_back(std::string(functions[i].name) + " = " + (key.empty() ? "unmapped" : key));
    }
    const size_t at = block_at.value_or(lines.size());
    if (at < lines.size() && !blank(lines[at])) {
        block.emplace_back();
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& line : lines) {
        out << line << '\n';
    }
    return !out.fail();
}

std::optional<KeyBindings> EditKeyBindings(HWND owner, const KeyBindings& keys, u32 control_type) {
    struct alignas(4) Template {
        DLGTEMPLATE header;
        WORD menu;
        WORD window_class;
        wchar_t title[16];
    };
    Template dialog_template{};
    dialog_template.header.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    wcscpy_s(dialog_template.title, L"Key Bindings");

    Editor editor;
    editor.keys = keys;
    editor.keys.resize(Game::Functions().size());
    editor.game_type = std::min(control_type, Game::TypeCount - 1);
    editor.type = editor.game_type;
    const INT_PTR result =
        DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &dialog_template.header, owner,
                                &EditorProc, reinterpret_cast<LPARAM>(&editor));
    DeleteObject(editor.font);
    if (result != IDOK) {
        return std::nullopt;
    }
    return editor.keys;
}

} // namespace Gr2Port::Launcher
