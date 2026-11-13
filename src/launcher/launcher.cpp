// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <windows.h>
// windows.h first
#include <commctrl.h>
#include <windowsx.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/string_util.h"
#include "core/language/language.h"
#include "launcher/art.h"
#include "launcher/driver_notice.h"
#include "launcher/folder_picker.h"
#include "launcher/launcher.h"
#include "launcher/setup_dialog.h"
#include "launcher/updater.h"

namespace Gr2Port::Launcher {

namespace {

constexpr const wchar_t* WindowClass = L"GR2portLauncher";
constexpr const wchar_t* Title = L"Gravity Rush 2";

// Pixels at 96 DPI, measured from Rise of the Tomb Raider's launcher.
constexpr int Width = 790;
constexpr int Height = 445;
constexpr int MenuFontSize = 16;
constexpr int MenuRight = 39;
constexpr int VersionRight = 27;
constexpr int VersionBaseline = 14;
constexpr int LogoLeft = 16;
constexpr int LogoBottom = 12;
constexpr int LogoWidth = 290;
// Menu text is white with a black outline, and all black under the pointer.
constexpr float MenuOutline = 2.5f;

enum class Item {
    Play,
    Options,
    Quit,
};

struct Entry {
    Item item;
    std::wstring_view text;
    int top;
};

// Entries 23 pixels apart and Quit 13 further down, as in the original.
constexpr std::array Entries{
    Entry{Item::Play, L"Play", 65},
    Entry{Item::Options, L"Options", 88},
    Entry{Item::Quit, L"Quit", 124},
};

// The timer the launcher looks at the updater's answer with, and its milliseconds.
constexpr UINT_PTR UpdateTimer = 1;
constexpr UINT UpdatePoll = 100;

struct State {
    const Art* art = nullptr;
    bool play = false;
    // The executable starts again, for a change of the Paths tab or an update to take effect.
    bool restart = false;
    // The updater's answer, asked for as the launcher opens.
    std::future<std::optional<Updater::Release>> found;
    std::wstring version;
    bool done = false;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    HBITMAP background = nullptr;
    HFONT menu_font = nullptr;
    HFONT version_font = nullptr;
    std::array<RECT, Entries.size()> item_rects{};
    int hot = -1;
    int pressed = -1;
    bool tracking = false;
};

int Scaled(const State& state, int value) {
    return MulDiv(value, static_cast<int>(state.dpi), USER_DEFAULT_SCREEN_DPI);
}

std::wstring VersionText() {
    const std::string_view revision{Common::g_scm_rev};
    std::wstring text = L"GR2port";
    if (!revision.empty()) {
        text += L" build ";
        for (const char c : revision.substr(0, 8)) {
            text += static_cast<wchar_t>(c);
        }
    }
    return text;
}

// The splash fills the window from its middle and darkens under the menu; the logo sits at the
// bottom left, and a grey line edges the window.
HBITMAP ComposeBackground(const Art& art, int width, int height, float scale) {
    return Art::Render(width, height, [&](Gdiplus::Graphics& graphics) {
        const float edge = std::max(1.0f, std::round(scale));
        const float full_width = static_cast<float>(width);
        const float full_height = static_cast<float>(height);
        const Gdiplus::RectF inner{edge, edge, full_width - 2.0f * edge, full_height - 2.0f * edge};
        const float part_width = art.SplashHeight() * inner.Width / inner.Height;
        art.DrawSplash(
            graphics, inner,
            {(art.SplashWidth() - part_width) / 2.0f, 0.0f, part_width, art.SplashHeight()});
        Art::Fade(graphics,
                  {inner.X + inner.Width * 0.58f, inner.Y, inner.Width * 0.42f, inner.Height}, 170);
        const float logo_width = LogoWidth * scale;
        const float logo_height = logo_width / art.LogoAspect();
        art.DrawLogo(graphics, {LogoLeft * scale, full_height - LogoBottom * scale - logo_height,
                                logo_width, logo_height});
        const Gdiplus::SolidBrush grey{Gdiplus::Color{255, 100, 100, 100}};
        graphics.FillRectangle(&grey, 0.0f, 0.0f, full_width, edge);
        graphics.FillRectangle(&grey, 0.0f, full_height - edge, full_width, edge);
        graphics.FillRectangle(&grey, 0.0f, 0.0f, edge, full_height);
        graphics.FillRectangle(&grey, full_width - edge, 0.0f, edge, full_height);
    });
}

// The menu's typeface: Verdana, as in the original, or one with the language's letters.
const wchar_t* MenuFace() {
    constexpr int Japanese = 0;
    constexpr int Korean = 9;
    constexpr int TraditionalChinese = 10;
    constexpr int SimplifiedChinese = 11;
    constexpr int Arabic = 21;
    constexpr int Thai = 27;
    switch (Language::Current()) {
    case Japanese:
        return L"Meiryo UI";
    case Korean:
        return L"Malgun Gothic";
    case TraditionalChinese:
        return L"Microsoft JhengHei UI";
    case SimplifiedChinese:
        return L"Microsoft YaHei UI";
    case Arabic:
        return L"Segoe UI";
    case Thai:
        return L"Leelawadee UI";
    default:
        return L"Verdana";
    }
}

HFONT MakeFont(int height, const wchar_t* face) {
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, face);
}

void Layout(HWND window, State& state) {
    DeleteObject(state.background);
    DeleteObject(state.menu_font);
    DeleteObject(state.version_font);
    RECT client{};
    GetClientRect(window, &client);
    const float scale = static_cast<float>(state.dpi) / USER_DEFAULT_SCREEN_DPI;
    state.background = ComposeBackground(*state.art, client.right, client.bottom, scale);
    state.menu_font = MakeFont(-Scaled(state, MenuFontSize), MenuFace());
    state.version_font = MakeFont(-MulDiv(8, static_cast<int>(state.dpi), 72), L"Segoe UI");

    const HDC dc = GetDC(window);
    const HGDIOBJ old_font = SelectObject(dc, state.menu_font);
    const int right = client.right - Scaled(state, MenuRight);
    for (size_t i = 0; i < Entries.size(); ++i) {
        SIZE size{};
        const std::wstring text = Language::Text(Entries[i].text);
        GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
        const int top = Scaled(state, Entries[i].top);
        state.item_rects[i] = RECT{right - size.cx, top, right, top + size.cy};
    }
    SelectObject(dc, old_font);
    ReleaseDC(window, dc);
}

// Centres the window on the primary monitor at the window's DPI.
void Center(HWND window, const State& state) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    const int width = Scaled(state, Width);
    const int height = Scaled(state, Height);
    const RECT& area = info.rcWork;
    SetWindowPos(window, nullptr, area.left + (area.right - area.left - width) / 2,
                 area.top + (area.bottom - area.top - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

int ItemAt(const State& state, POINT point) {
    for (size_t i = 0; i < state.item_rects.size(); ++i) {
        RECT area = state.item_rects[i];
        InflateRect(&area, Scaled(state, 4), Scaled(state, 1));
        if (PtInRect(&area, point)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SetHot(HWND window, State& state, int hot) {
    if (state.hot != hot) {
        state.hot = hot;
        InvalidateRect(window, nullptr, FALSE);
    }
}

void Paint(HWND window, const State& state) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const HDC frame = CreateCompatibleDC(dc);
    const HBITMAP frame_bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    const HGDIOBJ old_frame_bitmap = SelectObject(frame, frame_bitmap);
    const HDC art = CreateCompatibleDC(dc);
    const HGDIOBJ old_art_bitmap = SelectObject(art, state.background);
    BitBlt(frame, 0, 0, client.right, client.bottom, art, 0, 0, SRCCOPY);
    SelectObject(art, old_art_bitmap);
    DeleteDC(art);

    SetBkMode(frame, TRANSPARENT);
    {
        Gdiplus::Graphics graphics{frame};
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::FontFamily family{MenuFace()};
        const float scale = static_cast<float>(state.dpi) / USER_DEFAULT_SCREEN_DPI;
        Gdiplus::Pen outline{Gdiplus::Color{255, 0, 0, 0}, MenuOutline * scale};
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        const Gdiplus::SolidBrush white{Gdiplus::Color{255, 255, 255, 255}};
        const Gdiplus::SolidBrush black{Gdiplus::Color{255, 0, 0, 0}};
        for (size_t i = 0; i < Entries.size(); ++i) {
            Gdiplus::GraphicsPath path;
            const std::wstring text = Language::Text(Entries[i].text);
            path.AddString(text.data(), static_cast<INT>(text.size()), &family,
                           Gdiplus::FontStyleRegular,
                           static_cast<Gdiplus::REAL>(Scaled(state, MenuFontSize)),
                           Gdiplus::PointF{static_cast<Gdiplus::REAL>(state.item_rects[i].left),
                                           static_cast<Gdiplus::REAL>(state.item_rects[i].top)},
                           Gdiplus::StringFormat::GenericTypographic());
            graphics.DrawPath(&outline, &path);
            graphics.FillPath(static_cast<int>(i) == state.hot ? &black : &white, &path);
        }
    }
    const HGDIOBJ old_font = SelectObject(frame, state.version_font);
    SetTextColor(frame, RGB(255, 255, 255));
    SetTextAlign(frame, TA_RIGHT | TA_BASELINE);
    TextOutW(frame, client.right - Scaled(state, VersionRight),
             client.bottom - Scaled(state, VersionBaseline), state.version.c_str(),
             static_cast<int>(state.version.size()));
    SelectObject(frame, old_font);

    BitBlt(dc, 0, 0, client.right, client.bottom, frame, 0, 0, SRCCOPY);
    SelectObject(frame, old_frame_bitmap);
    DeleteObject(frame_bitmap);
    DeleteDC(frame);
    EndPaint(window, &paint);
}

// Tells what became of the request for the other user folder mode that this run started with.
void ShowFolderNotice(HWND owner) {
    const bool global = Common::FS::GetUserFolderMode() == Common::FS::UserFolderMode::Global;
    const std::wstring global_folder = Common::FS::GetGlobalUserFolder().wstring();
    const std::wstring portable_folder =
        (Common::FS::GetExecutableDirectory() / Common::FS::PORTABLE_DIR).wstring();
    // The folder in use, then the other.
    const std::array<std::wstring, 2> folders{global ? global_folder : portable_folder,
                                              global ? portable_folder : global_folder};
    std::wstring text;
    switch (Common::FS::GetUserFolderSwitch()) {
    case Common::FS::UserFolderSwitch::None:
        return;
    case Common::FS::UserFolderSwitch::Kept:
        text = Language::Text(L"A folder was already at\n{0}\nThe game uses it now. The folder "
                              L"at\n{1}\nstays where it is.",
                              folders);
        break;
    case Common::FS::UserFolderSwitch::GlobalMissing: {
        const std::array<std::wstring, 1> missing{global_folder};
        text = Language::Text(L"The global folder was not found at\n{0}\nA new portable userdata "
                              L"folder was created instead.",
                              missing);
        break;
    }
    case Common::FS::UserFolderSwitch::Failed: {
        const std::array<std::wstring, 2> moved{folders[1], folders[0]};
        text = Language::Text(L"The folder could not be moved to\n{0}\nThe game still uses\n{1}",
                              moved);
        break;
    }
    }
    MessageBoxW(owner, text.c_str(), Language::Text(L"User Data Folder").c_str(),
                MB_OK | MB_ICONWARNING);
}

// Shows a release's name and notes, and returns whether the player wants the update.
bool AskUpdate(HWND owner, const Updater::Release& release) {
    const std::wstring title = Language::Text(L"Update Available");
    const std::wstring name = Common::UTF8ToUTF16W(release.name);
    const std::wstring notes = Common::UTF8ToUTF16W(release.notes) + L"\n\n" +
                               Language::Text(L"Do you want to update now?");
    const std::wstring update = Language::Text(L"Update");
    const std::wstring later = Language::Text(L"Not Now");
    const std::array<TASKDIALOG_BUTTON, 2> buttons{
        TASKDIALOG_BUTTON{IDYES, update.c_str()},
        TASKDIALOG_BUTTON{IDNO, later.c_str()},
    };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    // Closing the window stands for Not Now.
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = name.c_str();
    config.pszContent = notes.c_str();
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = IDYES;
    int pressed = 0;
    return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == IDYES;
}

// An install as its window follows it.
struct Installing {
    std::shared_ptr<Updater::Download> download;
    std::future<bool> installed;
    bool done = false;
};

// The window's timer moves the bar and, once the install ends, closes the window as Cancel
// does; done tells the two apart.
HRESULT CALLBACK OnInstallEvent(HWND dialog, UINT notice, WPARAM wparam, LPARAM, LONG_PTR data) {
    auto& installing = *reinterpret_cast<Installing*>(data);
    if (notice == TDN_TIMER) {
        const u64 size = installing.download->size;
        if (size != 0) {
            SendMessageW(dialog, TDM_SET_PROGRESS_BAR_POS,
                         static_cast<WPARAM>(installing.download->received * 100 / size), 0);
        }
        if (!installing.done &&
            installing.installed.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            installing.done = true;
            SendMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
        }
    } else if (notice == TDN_BUTTON_CLICKED && wparam == IDCANCEL && !installing.done) {
        installing.download->stopped = true;
    }
    return S_OK;
}

// Downloads and installs a release behind a progress window. Returns whether its files are in
// place.
bool InstallUpdate(HWND owner, const Updater::Release& release) {
    Installing installing;
    installing.download = std::make_shared<Updater::Download>();
    installing.installed = Updater::Install(release, installing.download);
    const std::wstring title = Language::Text(L"Update Available");
    const std::wstring text = Language::Text(L"Downloading the update...");
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = title.c_str();
    config.pszContent = text.c_str();
    config.pfCallback = &OnInstallEvent;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&installing);
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    if (installing.download->stopped) {
        return false;
    }
    if (!installing.installed.get()) {
        MessageBoxW(owner, Language::Text(L"The update could not be installed.").c_str(),
                    title.c_str(), MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void Activate(HWND window, State& state, Item item) {
    switch (item) {
    case Item::Play:
        state.play = true;
        DestroyWindow(window);
        break;
    case Item::Options:
        if (ShowSetup(window, *state.art)) {
            state.restart = true;
            DestroyWindow(window);
            break;
        }
        // The menu takes the language Setup left.
        Layout(window, state);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case Item::Quit:
        state.play = false;
        DestroyWindow(window);
        break;
    }
}

void OnKey(HWND window, State& state, WPARAM key) {
    const int count = static_cast<int>(Entries.size());
    switch (key) {
    case VK_UP:
        SetHot(window, state, state.hot <= 0 ? count - 1 : state.hot - 1);
        break;
    case VK_DOWN:
        SetHot(window, state, state.hot < 0 ? 0 : (state.hot + 1) % count);
        break;
    case VK_RETURN:
        Activate(window, state, state.hot >= 0 ? Entries[state.hot].item : Item::Play);
        break;
    case VK_ESCAPE:
        Activate(window, state, Item::Quit);
        break;
    default:
        break;
    }
}

// The art drags the window, as a title bar would; only the entries are client area.
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* const state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        return ItemAt(*state, point) >= 0 ? HTCLIENT : HTCAPTION;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            SetCursor(LoadCursorA(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (!state->tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            state->tracking = TrackMouseEvent(&track) != FALSE;
        }
        SetHot(window, *state, ItemAt(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}));
        return 0;
    case WM_MOUSELEAVE:
        state->tracking = false;
        SetHot(window, *state, -1);
        return 0;
    case WM_LBUTTONDOWN:
        state->pressed = ItemAt(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (state->pressed >= 0) {
            SetCapture(window);
        }
        return 0;
    case WM_LBUTTONUP: {
        const int pressed = std::exchange(state->pressed, -1);
        ReleaseCapture();
        const int item = ItemAt(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (item >= 0 && item == pressed) {
            Activate(window, *state, Entries[item].item);
        }
        return 0;
    }
    case WM_KEYDOWN:
        OnKey(window, *state, wparam);
        return 0;
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam);
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(window, *state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(window, *state);
        return 0;
    case WM_TIMER:
        // The offer waits for a dialog over the launcher to close.
        if (IsWindowEnabled(window) &&
            state->found.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            KillTimer(window, UpdateTimer);
            const std::optional<Updater::Release> release = state->found.get();
            if (release && AskUpdate(window, *release) && InstallUpdate(window, *release)) {
                state->restart = true;
                DestroyWindow(window);
            }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool Missing(DWORD id) {
    const HANDLE handle = GetStdHandle(id);
    return handle == nullptr || handle == INVALID_HANDLE_VALUE;
}

// The link of the message that no game is found closes the message as OK does, and choose tells
// the two apart.
HRESULT CALLBACK OnNoGameEvent(HWND dialog, UINT notice, WPARAM, LPARAM, LONG_PTR choose) {
    if (notice == TDN_HYPERLINK_CLICKED) {
        *reinterpret_cast<bool*>(choose) = true;
        SendMessageW(dialog, TDM_CLICK_BUTTON, IDOK, 0);
    }
    return S_OK;
}

} // namespace

void AttachTerminal() {
    const bool out = Missing(STD_OUTPUT_HANDLE);
    const bool err = Missing(STD_ERROR_HANDLE);
    if ((!out && !err) || !AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    const HANDLE console =
        CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        return;
    }
    FILE* stream = nullptr;
    if (out) {
        SetStdHandle(STD_OUTPUT_HANDLE, console);
        freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    if (err) {
        SetStdHandle(STD_ERROR_HANDLE, console);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}

std::optional<std::filesystem::path> AskGameFolder(const std::string& text) {
    // The message and the picker draw at the monitor's own scale, as the launcher's windows do.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring content = Common::UTF8ToUTF16W(text);
    const std::wstring link =
        L"<a href=\"folder\">" + Language::Text(L"Choose custom game directory instead") + L"</a>";
    bool choose = false;
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ENABLE_HYPERLINKS;
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    config.pszWindowTitle = Title;
    config.pszMainIcon = TD_ERROR_ICON;
    config.pszContent = content.c_str();
    // The footer is the dialog's last row, under the OK button.
    config.pszFooter = link.c_str();
    config.pfCallback = &OnNoGameEvent;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&choose);
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    if (!choose) {
        return std::nullopt;
    }
    return PickFolder(nullptr,
                      Language::Text(L"Choose the folder that holds your game and its update"));
}

bool Run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool play = true;
    bool restart = false;
    {
        const Art art;
        State state;
        state.art = &art;
        state.version = VersionText();

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &WindowProc;
        window_class.hInstance = instance;
        window_class.hIcon = LoadIconW(instance, L"IDI_ICON1");
        window_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        window_class.lpszClassName = WindowClass;
        RegisterClassExW(&window_class);

        const HWND window = CreateWindowExW(WS_EX_APPWINDOW, WindowClass, Title,
                                            WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0, Width,
                                            Height, nullptr, nullptr, instance, &state);
        if (window == nullptr) {
            LOG_ERROR(Frontend, "GR2port: no launcher window, starting the game");
        } else {
            state.dpi = GetDpiForWindow(window);
            Center(window, state);
            Layout(window, state);
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            ShowDriverNotice(window);
            ShowFolderNotice(window);
            // The updater's answer comes on a thread of its own; the window looks for it.
            state.found = Updater::Check();
            SetTimer(window, UpdateTimer, UpdatePoll, nullptr);
            MSG message{};
            while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            play = state.play;
            restart = state.restart;
        }
        DeleteObject(state.background);
        DeleteObject(state.menu_font);
        DeleteObject(state.version_font);
        UnregisterClassW(WindowClass, instance);
    }
    if (restart) {
        Common::FS::RestartExecutable();
        return false;
    }
    return play;
}

} // namespace Gr2Port::Launcher
