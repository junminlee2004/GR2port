// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <windows.h>
// windows.h first
#include <windowsx.h>

#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "launcher/art.h"
#include "launcher/launcher.h"
#include "launcher/setup_dialog.h"

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

struct State {
    const Art* art = nullptr;
    bool play = false;
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
    state.menu_font = MakeFont(-Scaled(state, MenuFontSize), L"Verdana");
    state.version_font = MakeFont(-MulDiv(8, static_cast<int>(state.dpi), 72), L"Segoe UI");

    const HDC dc = GetDC(window);
    const HGDIOBJ old_font = SelectObject(dc, state.menu_font);
    const int right = client.right - Scaled(state, MenuRight);
    for (size_t i = 0; i < Entries.size(); ++i) {
        SIZE size{};
        GetTextExtentPoint32W(dc, Entries[i].text.data(), static_cast<int>(Entries[i].text.size()),
                              &size);
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
        const Gdiplus::FontFamily family{L"Verdana"};
        const float scale = static_cast<float>(state.dpi) / USER_DEFAULT_SCREEN_DPI;
        Gdiplus::Pen outline{Gdiplus::Color{255, 0, 0, 0}, MenuOutline * scale};
        outline.SetLineJoin(Gdiplus::LineJoinRound);
        const Gdiplus::SolidBrush white{Gdiplus::Color{255, 255, 255, 255}};
        const Gdiplus::SolidBrush black{Gdiplus::Color{255, 0, 0, 0}};
        for (size_t i = 0; i < Entries.size(); ++i) {
            Gdiplus::GraphicsPath path;
            path.AddString(Entries[i].text.data(), static_cast<INT>(Entries[i].text.size()),
                           &family, Gdiplus::FontStyleRegular,
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

void Activate(HWND window, State& state, Item item) {
    switch (item) {
    case Item::Play:
        state.play = true;
        DestroyWindow(window);
        break;
    case Item::Options:
        ShowSetup(window, *state.art);
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

bool Run() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool play = true;
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
            MSG message{};
            while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            play = state.play;
        }
        DeleteObject(state.background);
        DeleteObject(state.menu_font);
        DeleteObject(state.version_font);
        UnregisterClassW(WindowClass, instance);
    }
    return play;
}

} // namespace Gr2Port::Launcher
