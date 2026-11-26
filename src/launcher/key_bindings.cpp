// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <windows.h>
// windows.h first
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <commctrl.h>

#include "core/language/language.h"
#include "input/game_functions.h"
#include "input/input_handler.h"
#include "input/pad_bindings.h"
#include "launcher/key_bindings.h"
#include "launcher/pad_commands.h"

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
constexpr RECT ChangeArea{10, 398, 110, 420};
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
    // The Button Bindings dialog edits buttons and back paddles, named as layout's prompts show
    // them.
    bool pad = false;
    Input::PadBindings::Map buttons;
    Input::PadBindings::Map paddles;
    // The photo functions' pad bindings (Input::PadBindings::PhotoFunctions), compact.
    Input::PadBindings::Map photo;
    std::string layout;
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

// ------------------------------------------------------------------------------ pad buttons

// A pad button by the input config's input name; triggers are axes.
struct PadInput {
    std::string_view name;
    SDL_GamepadButton button;
    SDL_GamepadAxis axis;
};

constexpr std::array<PadInput, 20> PadInputs{{
    {"cross", SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_AXIS_INVALID},
    {"circle", SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_AXIS_INVALID},
    {"square", SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_AXIS_INVALID},
    {"triangle", SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_AXIS_INVALID},
    {"l1", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_AXIS_INVALID},
    {"r1", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_AXIS_INVALID},
    {"l2", SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_LEFT_TRIGGER},
    {"r2", SDL_GAMEPAD_BUTTON_INVALID, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER},
    {"l3", SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_AXIS_INVALID},
    {"r3", SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_AXIS_INVALID},
    {"options", SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_AXIS_INVALID},
    {"back", SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_AXIS_INVALID},
    {"pad_up", SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_AXIS_INVALID},
    {"pad_down", SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_AXIS_INVALID},
    {"pad_left", SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_AXIS_INVALID},
    {"pad_right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_AXIS_INVALID},
    {"lpaddle_high", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, SDL_GAMEPAD_AXIS_INVALID},
    {"rpaddle_high", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_AXIS_INVALID},
    {"lpaddle_low", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, SDL_GAMEPAD_AXIS_INVALID},
    {"rpaddle_low", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, SDL_GAMEPAD_AXIS_INVALID},
}};

// A pad button's name as the layout's prompts show it: the face buttons by position, the rest by
// their print on that pad.
std::wstring PadText(std::string_view layout, std::string_view name) {
    if (name.empty()) {
        return L"\x2014";
    }
    struct Names {
        std::string_view layout;
        std::array<const wchar_t*, 16> text;
    };
    // cross circle square triangle l1 r1 l2 r2 l3 r3 options back, then the back paddles: upper
    // left, upper right, lower left, lower right
    static constexpr std::array<Names, 5> Layouts{{
        {"xbox",
         {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"LS", L"RS", L"Menu", L"View", L"P3",
          L"P1", L"P4", L"P2"}},
        {"switch",
         {L"B", L"A", L"Y", L"X", L"L", L"R", L"ZL", L"ZR", L"LS", L"RS", L"+", L"\x2212", L"GL",
          L"GR", L"SR", L"SL"}},
        {"steamdeck",
         {L"A", L"B", L"X", L"Y", L"L1", L"R1", L"L2", L"R2", L"L3", L"R3", L"Menu", L"View", L"L4",
          L"R4", L"L5", L"R5"}},
        {"steamcontroller",
         {L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"Stick", L"Right Pad", L"Start",
          L"Back", L"L4", L"R4", L"L5", L"R5"}},
        {"playstation",
         {L"Cross", L"Circle", L"Square", L"Triangle", L"L1", L"R1", L"L2", L"R2", L"L3", L"R3",
          L"OPTIONS", L"SHARE", L"LB", L"RB", L"L Fn", L"R Fn"}},
    }};
    static constexpr std::array<std::string_view, 16> Order{
        "cross",        "circle",       "square",      "triangle",   "l1",      "r1",
        "l2",           "r2",           "l3",          "r3",         "options", "back",
        "lpaddle_high", "rpaddle_high", "lpaddle_low", "rpaddle_low"};
    const auto names = std::ranges::find(Layouts, layout, &Names::layout);
    const Names& chosen = names != Layouts.end() ? *names : Layouts.back();
    if (const auto at = std::ranges::find(Order, name); at != Order.end()) {
        return Tr(chosen.text[at - Order.begin()]);
    }
    if (name == "pad_up") {
        return Tr(L"D-pad up");
    }
    if (name == "pad_down") {
        return Tr(L"D-pad down");
    }
    if (name == "pad_left") {
        return Tr(L"D-pad left");
    }
    if (name == "pad_right") {
        return Tr(L"D-pad right");
    }
    return Wide(name);
}

// The PlayStation button a function is on in a control type, or empty when the type uses a
// stick, a touch pad flick or a button held with another.
std::string_view GameButton(const Game::Function& function, u32 type) {
    const auto buttons = Input::PadBindings::GameButtons();
    if (std::ranges::find(buttons, function.output) != buttons.end()) {
        return function.output;
    }
    if (function.name == "gr2_evade" && type == 0) {
        return "r2";
    }
    if (function.name == "gr2_normal_style") {
        return type == 0 ? "touchpad_center" : type >= 2 ? "r2" : "";
    }
    return {};
}

// ------------------------------------------------------------------------------- key capture

struct Capture {
    std::wstring function;
    HFONT font = nullptr;
    // Takes a pad button instead of a key or mouse button.
    bool pad = false;
    // Takes up to three keys or pad buttons held together, once they are all released.
    bool combination = false;
    // Takes two pad buttons held together or one alone, for a photo function.
    bool photo = false;
    int seconds_left = CaptureSeconds;
    bool done = false;
    std::string key;
    std::vector<SDL_Gamepad*> pads;
    // Pad buttons held since before the popup opened, which count once released.
    std::set<std::string_view> held;
    // A combination's inputs in the order pressed, and the inputs held now.
    std::vector<std::string> inputs;
    std::set<std::string> down;
};

constexpr size_t CombinationInputs = 3;

constexpr UINT_PTR CountdownTimer = 1;
constexpr UINT_PTR PadTimer = 2;
constexpr UINT PadPollMs = 15;
// A trigger counts as pressed past half its travel.
constexpr Sint16 TriggerPressed = 16384;

void Finish(Capture& capture, std::string key) {
    if (!capture.done &&
        (key.empty() || capture.pad || Input::string_to_keyboard_key_map.contains(key))) {
        capture.key = std::move(key);
        capture.done = true;
    }
}

// Adds an input to the combination; a key the input handler does not know is passed over.
void Press(Capture& capture, std::string input) {
    if (input.empty() || (!capture.pad && !Input::string_to_keyboard_key_map.contains(input))) {
        return;
    }
    const size_t most = capture.photo ? 2 : CombinationInputs;
    if (capture.inputs.size() < most &&
        std::ranges::find(capture.inputs, input) == capture.inputs.end()) {
        capture.inputs.push_back(input);
    }
    capture.down.insert(std::move(input));
}

// Takes the combination as an input config line's inputs once nothing is held.
void Release(Capture& capture, const std::string& input) {
    capture.down.erase(input);
    if (capture.done || !capture.down.empty() || capture.inputs.empty()) {
        return;
    }
    for (const std::string& name : capture.inputs) {
        capture.key += capture.key.empty() ? name : ", " + name;
    }
    capture.done = true;
}

// The pad buttons pressed now on any open pad.
std::set<std::string_view> PressedPadInputs(const Capture& capture) {
    SDL_UpdateGamepads();
    std::set<std::string_view> pressed;
    for (SDL_Gamepad* const pad : capture.pads) {
        for (const PadInput& input : PadInputs) {
            const bool down = input.button != SDL_GAMEPAD_BUTTON_INVALID
                                  ? SDL_GetGamepadButton(pad, input.button)
                                  : SDL_GetGamepadAxis(pad, input.axis) > TriggerPressed;
            if (down) {
                pressed.insert(input.name);
            }
        }
    }
    return pressed;
}

void PollPads(Capture& capture) {
    const std::set<std::string_view> pressed = PressedPadInputs(capture);
    std::erase_if(capture.held, [&](std::string_view name) { return !pressed.contains(name); });
    if (capture.combination) {
        for (const std::string_view name : pressed) {
            if (!capture.held.contains(name)) {
                Press(capture, std::string(name));
            }
        }
        for (const std::string& name : std::set{capture.down}) {
            if (!pressed.contains(name)) {
                Release(capture, name);
            }
        }
        return;
    }
    for (const std::string_view name : pressed) {
        if (!capture.held.contains(name)) {
            Finish(capture, std::string(name));
            return;
        }
    }
}

// A popup that takes the next key or mouse button, or nothing when the countdown ends.
LRESULT CALLBACK CaptureProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* const capture = reinterpret_cast<Capture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (capture == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (capture->pad) {
        // Escape gives up; other keys and the mouse do nothing.
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
            capture->done = true;
            return 0;
        }
        if ((message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
            (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)) {
            return 0;
        }
    } else if (capture->combination) {
        // Keys only: the mouse does nothing.
        switch (message) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            Press(*capture, KeyName(wparam, lparam));
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            Release(*capture, KeyName(wparam, lparam));
            return 0;
        default:
            if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
                return 0;
            }
            break;
        }
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
        if (wparam == PadTimer) {
            PollPads(*capture);
            return 0;
        }
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
        const std::vector<std::wstring> values{capture->function,
                                               std::to_wstring(capture->seconds_left)};
        const std::wstring text =
            capture->photo
                ? Tr(L"Hold down two buttons on the controller together to bind a combination to\n"
                     L"{0}\nor press a back paddle.\n\nNothing changes in {1} s.",
                     values)
            : capture->combination
                ? (capture->pad ? Tr(L"Hold up to three buttons on the controller together for\n"
                                     L"{0}\n\nNothing changes in {1} s.",
                                     values)
                                : Tr(L"Hold up to three keys together for\n{0}\n\nNothing "
                                     L"changes in {1} s.",
                                     values))
            : capture->pad
                ? Tr(L"Press a button on the controller for\n{0}\n\nNothing changes in {1} s.",
                     values)
                : Tr(L"Press a key or mouse button for\n{0}\n\nNothing changes in {1} s.", values);
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

// Waits for a key or mouse button over owner, or a pad button when pad is set, or a combination of
// them when combination is set, or two pad buttons or one for a photo function when photo is set.
// Empty when the countdown ends first.
std::string CaptureKey(HWND owner, const Editor& editor, const std::wstring& function, bool pad,
                       bool combination = false, bool photo = false) {
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
    // The pads move no window while the popup reads them.
    const PadCommands::Pause pause;
    Capture capture{function, editor.font, pad, combination || photo, photo};
    const bool gamepads = pad && SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (gamepads) {
        int count = 0;
        SDL_JoystickID* const ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count; ++i) {
            if (SDL_Gamepad* const opened = SDL_OpenGamepad(ids[i])) {
                capture.pads.push_back(opened);
            }
        }
        SDL_free(ids);
        capture.held = PressedPadInputs(capture);
        SetTimer(window, PadTimer, PadPollMs, nullptr);
    }
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&capture));
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    SetCapture(window);
    SetTimer(window, CountdownTimer, 1000, nullptr);
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
    KillTimer(window, CountdownTimer);
    KillTimer(window, PadTimer);
    for (SDL_Gamepad* const opened : capture.pads) {
        SDL_CloseGamepad(opened);
    }
    if (gamepads) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
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

// The input config name of a photo function's pad binding, after its key line's
// ("gr2_hide_ui" -> "gr2_hide_ui_pad"); empty for a function without one.
std::string PhotoName(const Game::Function& function) {
    std::string name = std::string(function.name) + "_pad";
    const auto photo = Input::PadBindings::PhotoFunctions();
    return std::ranges::find(photo, name) != photo.end() ? name : std::string{};
}

// A photo function's name in the launcher, for the status line.
std::wstring PhotoLabel(std::string_view photo) {
    for (const Game::Function& function : Game::Functions()) {
        if (PhotoName(function) == photo) {
            return Tr(Wide(function.label));
        }
    }
    return {};
}

// A photo function's binding as the layout's prompts name it ("LS + RT"), or a dash for none.
std::wstring PhotoText(const Editor& editor, const std::string& photo, std::string_view layout) {
    const auto bound = editor.photo.find(photo);
    return bound == editor.photo.end() || bound->second.empty()
               ? PadText(layout, {})
               : CombinationText(bound->second, true, layout);
}

// The pad button bound to the PlayStation button a function is on in the type shown, then the
// back paddles that press it too, or a photo function's binding; blank for a function on a stick,
// a flick or a combination.
std::wstring PadCell(const Editor& editor, const Game::Function& function) {
    if (const std::string photo = PhotoName(function); !photo.empty()) {
        return PhotoText(editor, photo, editor.layout);
    }
    const std::string_view button = GameButton(function, editor.type);
    if (button.empty()) {
        return {};
    }
    std::wstring names;
    if (const auto bound = editor.buttons.find(button);
        bound != editor.buttons.end() && !bound->second.empty()) {
        names = PadText(editor.layout, bound->second);
    }
    for (const std::string_view paddle : Input::PadBindings::Paddles()) {
        if (const auto bound = editor.paddles.find(paddle);
            bound != editor.paddles.end() && bound->second == button) {
            names += (names.empty() ? L"" : L", ") + PadText(editor.layout, paddle);
        }
    }
    return names.empty() ? PadText(editor.layout, {}) : names;
}

// The pad column shows the type's pad inputs, and a photo function's binding by its PlayStation
// names; the key column is the same in every type, and the button column shows the pad button of
// each function's PlayStation button.
void ShowRows(const Editor& editor) {
    const auto functions = Game::Functions();
    for (size_t i = 0; i < functions.size(); ++i) {
        const std::string photo = PhotoName(functions[i]);
        SetCell(editor.list, static_cast<int>(i), 1,
                !photo.empty() && editor.photo.contains(photo)
                    ? PhotoText(editor, photo, "playstation")
                    : Tr(Wide(functions[i].pad[editor.type])));
        SetCell(editor.list, static_cast<int>(i), 2,
                editor.pad ? PadCell(editor, functions[i]) : KeyText(editor.keys[i]));
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

// The functions on a PlayStation button in the type shown, for the status line.
std::wstring FunctionsOn(const Editor& editor, std::string_view button) {
    std::wstring names;
    for (const Game::Function& function : Game::Functions()) {
        if (GameButton(function, editor.type) == button) {
            names += (names.empty() ? L"" : L", ") + Tr(Wide(function.label));
        }
    }
    return names;
}

// A back paddle given to something else no longer does the photo function it did alone; adds
// those functions to the status line.
void FreePaddle(Editor& editor, std::string_view paddle, std::wstring& status) {
    for (auto& [photo, bound] : editor.photo) {
        if (bound == paddle) {
            bound.clear();
            status += L" " + Tr(L"{0} has no button now.", {PhotoLabel(photo)});
        }
    }
}

// Takes two pad buttons held together, the first held while the second is pressed, or a back
// paddle, for a photo function. A binding another photo function had moves to this one, and a
// back paddle stops pressing the PlayStation button it pressed.
void ChangePhotoBinding(HWND window, Editor& editor, const std::string& photo,
                        const std::wstring& label) {
    std::string inputs = CaptureKey(window, editor, label, true, false, true);
    std::erase_if(inputs, [](unsigned char c) { return std::isspace(c) != 0; });
    if (inputs.empty()) {
        ShowStatus(window, Tr(L"No button was pressed; {0} keeps its button.", {label}));
        return;
    }
    if (!Input::PadBindings::IsPhotoBinding(inputs)) {
        ShowStatus(window, Tr(L"{0} takes two buttons held together or a back paddle.", {label}));
        return;
    }
    std::wstring status =
        Tr(L"{0} now does {1}.", {CombinationText(inputs, true, editor.layout), label});
    for (auto& [other, bound] : editor.photo) {
        if (other != photo && bound == inputs) {
            bound.clear();
            status += L" " + Tr(L"{0} has no button now.", {PhotoLabel(other)});
        }
    }
    if (const auto paddle = editor.paddles.find(inputs); paddle != editor.paddles.end()) {
        status += L" " + Tr(L"{0} no longer does {1}.",
                            {PadText(editor.layout, inputs), FunctionsOn(editor, paddle->second)});
        editor.paddles.erase(paddle);
    }
    editor.photo[photo] = std::move(inputs);
    ShowRows(editor);
    ShowStatus(window, status);
}

// Takes a pad button for the selected function's PlayStation button. A pad button that another
// PlayStation button had moves to this one, and a back paddle presses it beside its pad button.
void ChangeButton(HWND window, Editor& editor, int row) {
    const Game::Function& function = Game::Functions()[row];
    const std::wstring label = Tr(Wide(function.label));
    if (const std::string photo = PhotoName(function); !photo.empty()) {
        ChangePhotoBinding(window, editor, photo, label);
        return;
    }
    const std::string_view button = GameButton(function, editor.type);
    if (button.empty()) {
        ShowStatus(window,
                   Tr(L"{0} is not on one button in {1}.", {label, Tr(TypeNames[editor.type])}));
        return;
    }
    const std::string input = CaptureKey(window, editor, label, true);
    if (input.empty()) {
        ShowStatus(window, Tr(L"No button was pressed; {0} keeps its button.", {label}));
        return;
    }
    if (Input::PadBindings::IsPaddle(input)) {
        editor.paddles[input] = std::string(button);
        std::wstring status = Tr(L"{0} now also does {1}.", {PadText(editor.layout, input), label});
        FreePaddle(editor, input, status);
        ShowRows(editor);
        ShowStatus(window, status);
        return;
    }
    std::wstring status = Tr(L"{0} now does {1}.", {PadText(editor.layout, input), label});
    for (auto& [other, bound] : editor.buttons) {
        if (other != button && bound == input) {
            bound.clear();
            status += L" " + Tr(L"{0} has no button now.", {FunctionsOn(editor, other)});
        }
    }
    editor.buttons[std::string(button)] = input;
    ShowRows(editor);
    ShowStatus(window, status);
}

// Takes a key for the selected function. A key that another function had moves to this one.
void ChangeKey(HWND window, Editor& editor) {
    const int row = SelectedRow(editor);
    const auto functions = Game::Functions();
    if (row < 0 || row >= static_cast<int>(functions.size())) {
        ShowStatus(window, Tr(L"Pick a game function first."));
        return;
    }
    if (editor.pad) {
        ChangeButton(window, editor, row);
        return;
    }
    const std::wstring label = Tr(Wide(functions[row].label));
    const std::string key = CaptureKey(window, editor, label, false);
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
    SetWindowTextW(window, Tr(editor.pad ? L"Button Bindings" : L"Key Bindings").c_str());
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
    const std::array<const wchar_t*, 3> Headers{L"Game Function", L"Controller",
                                                editor.pad ? L"Button" : L"Key"};
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

    const std::wstring in_use =
        editor.pad ? Tr(L"The game used {0} last (Options > Controls). A button presses the same "
                        L"PlayStation button in every type.",
                        {Tr(TypeNames[editor.game_type])})
                   : Tr(L"The game used {0} last (Options > Controls). Each key does "
                        L"its function in every type.",
                        {Tr(TypeNames[editor.game_type])});
    MakeControl(editor, window, WC_STATICW, in_use.c_str(), SS_LEFT, IdType, TypeLine);
    MakeControl(editor, window, WC_STATICW,
                (editor.pad ? Tr(L"Double-click a function, then press its button on the "
                                 L"controller.")
                            : Tr(L"Double-click a function, then press its key. Mouse buttons and "
                                 L"the wheel count."))
                    .c_str(),
                SS_LEFT, IdStatus, StatusLine);
    MakeControl(editor, window, WC_BUTTONW,
                Tr(editor.pad ? L"Change Button..." : L"Change Key...").c_str(),
                BS_PUSHBUTTON | WS_TABSTOP, IdChange, ChangeArea);
    MakeControl(editor, window, WC_BUTTONW, Tr(editor.pad ? L"Clear Button" : L"Clear Key").c_str(),
                BS_PUSHBUTTON | WS_TABSTOP, IdClear, ClearButton);
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
                if (!editor->pad) {
                    editor->keys[row].clear();
                } else if (const std::string photo = PhotoName(Game::Functions()[row]);
                           !photo.empty()) {
                    editor->photo[photo].clear();
                } else if (const std::string_view button =
                               GameButton(Game::Functions()[row], editor->type);
                           !button.empty()) {
                    editor->buttons[std::string(button)].clear();
                    std::erase_if(editor->paddles,
                                  [&](const auto& paddle) { return paddle.second == button; });
                }
                ShowRows(*editor);
            }
            return TRUE;
        case IdDefaults:
            if (editor->pad) {
                editor->buttons = DefaultPadBindings().buttons;
                editor->paddles.clear();
                editor->photo = DefaultPadBindings().photo;
            } else {
                editor->keys = DefaultKeyBindings();
            }
            ShowRows(*editor);
            ShowStatus(window, Tr(editor->pad ? L"Every function has its default button again."
                                              : L"Every function has its default key again."));
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

std::optional<KeyBindings> EditKeyBindings(HWND owner, const KeyBindings& keys, u32 control_type,
                                           const PadBindings& pad) {
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
    editor.photo = pad.photo;
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

std::optional<PadBindings> EditPadBindings(HWND owner, const PadBindings& bindings,
                                           u32 control_type, std::string_view layout) {
    struct alignas(4) Template {
        DLGTEMPLATE header;
        WORD menu;
        WORD window_class;
        wchar_t title[16];
    };
    Template dialog_template{};
    dialog_template.header.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    wcscpy_s(dialog_template.title, L"Button Bindings");

    Editor editor;
    editor.pad = true;
    editor.buttons = bindings.buttons;
    editor.paddles = bindings.paddles;
    editor.photo = bindings.photo;
    editor.layout = layout;
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
    return PadBindings{editor.buttons, editor.paddles, editor.photo};
}

std::string CaptureCombination(HWND owner, HFONT font, UINT dpi, const std::wstring& function,
                               bool pad) {
    Editor editor;
    editor.font = font;
    editor.dpi = dpi;
    return CaptureKey(owner, editor, function, pad, true);
}

std::wstring CombinationText(std::string_view inputs, bool pad, std::string_view layout) {
    const std::string compact = Compact(inputs);
    if (compact.empty()) {
        return Tr(L"None");
    }
    std::wstring text;
    for (const auto part : std::views::split(compact, ',')) {
        const std::string_view name(part.begin(), part.end());
        text += (text.empty() ? L"" : L" + ") + (pad ? PadText(layout, name) : KeyText(name));
    }
    return text;
}

} // namespace Gr2Port::Launcher
