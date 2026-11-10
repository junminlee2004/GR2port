// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <windows.h>
// windows.h first
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <commctrl.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/game_check/game_check.h"
#include "core/game_check/game_patches.h"
#include "core/prompt_packs/prompt_packs.h"
#include "core/user_settings.h"
#include "input/input_handler.h"
#include "input/selected_pad.h"
#include "launcher/art.h"
#include "launcher/avatar.h"
#include "launcher/key_bindings.h"
#include "launcher/setup_dialog.h"
#include "video_core/gr2port/settings.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Gr2Port::Launcher {

namespace {

constexpr const wchar_t* PageClass = L"GR2portSetupPage";

// Pixels at 96 DPI in the dialog's client area, measured from Rise of the Tomb Raider's Setup
// dialog.
constexpr int ClientWidth = 602;
constexpr int ClientHeight = 461;
constexpr int BannerHeight = 50;
constexpr RECT TabArea{9, 59, 595, 434};
constexpr int CheckLeft = 198;
constexpr int CheckWidth = 220;
constexpr int CheckHeight = 17;
constexpr int FullscreenTop = 88;
constexpr int ExclusiveTop = 111;
constexpr int LabelLeft = 18;
constexpr int LabelWidth = 170;
constexpr int ComboLeft = 198;
constexpr int ComboWidth = 225;
constexpr int ComboHeight = 22;
constexpr int ComboListHeight = 240;
constexpr RECT DefaultsButton{10, 437, 143, 459};
constexpr RECT OkButton{433, 437, 506, 459};
constexpr RECT CancelButton{514, 437, 587, 459};

enum Control : int {
    IdFullscreen = 1001,
    IdExclusive,
    IdMonitorLabel,
    IdMonitor,
    IdResolutionLabel,
    IdResolution,
    IdRefreshLabel,
    IdRefresh,
    IdVSyncLabel,
    IdVSync,
    IdFrameRateLabel,
    IdFrameRate,
    IdAspectLabel,
    IdAspect,
    IdRenderSizeLabel,
    IdRenderSize,
    IdDlc,
    IdHalloween,
    IdSignUp,
    IdOnlineIdLabel,
    IdOnlineId,
    IdPasswordLabel,
    IdPassword,
    IdAvatarLabel,
    IdAvatar,
    IdAvatarChoose,
    IdAvatarNote,
    IdTrophies,
    IdControllerGroup,
    IdMotion,
    IdGyroSwap,
    IdGyroYaw,
    IdGyroPitch,
    IdGyroRoll,
    IdVibration,
    IdBackgroundInput,
    IdKeyboard,
    IdActivePadGroup,
    IdActivePad,
    IdActivePadId,
    IdDefaultPadGroup,
    IdDefaultPadName,
    IdDefaultPadId,
    IdSetDefaultPad,
    IdRemoveDefaultPad,
    IdLeftStickGroup,
    IdLeftMinLabel,
    IdLeftMin,
    IdLeftMinValue,
    IdLeftMaxLabel,
    IdLeftMax,
    IdLeftMaxValue,
    IdRightStickGroup,
    IdRightMinLabel,
    IdRightMin,
    IdRightMinValue,
    IdRightMaxLabel,
    IdRightMax,
    IdRightMaxValue,
    IdFootGroup,
    IdFootHorizontalLabel,
    IdFootHorizontal,
    IdFootHorizontalValue,
    IdFootVerticalLabel,
    IdFootVertical,
    IdFootVerticalValue,
    IdShiftGroup,
    IdShiftHorizontalLabel,
    IdShiftHorizontal,
    IdShiftHorizontalValue,
    IdShiftVerticalLabel,
    IdShiftVertical,
    IdShiftVerticalValue,
    IdPromptsLabel,
    IdPrompts,
    IdPadPromptsLabel,
    IdPadPrompts,
    IdMouseNote,
    IdTouchSwipe,
    IdTouchSwipeNote,
    IdKeyBindings,
    IdExtras,
    IdPresetLabel,
    IdPreset,
    IdShadowsLabel,
    IdShadows,
    IdFogLabel,
    IdFog,
    IdOcclusionLabel,
    IdOcclusion,
    IdMotionBlurLabel,
    IdMotionBlur,
    IdAntialiasLabel,
    IdAntialias,
    IdBloomLabel,
    IdBloom,
    IdTextureStreamingLabel,
    IdTextureStreaming,
    IdGraphicsNote,
    IdDefaults,
};

enum Page : size_t {
    DisplayPage,
    GraphicsPage,
    ControllerPage,
    MousePage,
    DlcPage,
    OnlinePage,
    TrophiesPage,
    ExtrasPage,
    PageCount,
};

// Tab controls draw a single ampersand as an underline.
constexpr std::array<const wchar_t*, PageCount> PageNames{
    L"Display", L"Graphics", L"Controller", L"Mouse && Keyboard",
    L"DLC",     L"Online",   L"Trophies",   L"Cheats && Extras"};

constexpr RECT ExtrasList{18, 88, 577, 424};
constexpr int OnlineIdTop = 121;
constexpr int PasswordTop = 150;
constexpr int AvatarTop = 192;
constexpr RECT AvatarChooseButton{338, 252, 448, 274};

// The Controller tab: GR2Launcher's controller settings in two columns of group boxes.
constexpr RECT ControllerGroup{18, 88, 290, 280};
constexpr RECT ActivePadGroup{300, 88, 577, 154};
constexpr RECT DefaultPadGroup{300, 160, 577, 280};
constexpr RECT SetDefaultPadButton{312, 220, 565, 243};
constexpr RECT RemoveDefaultPadButton{312, 249, 565, 272};
constexpr int GroupInset = 12;
constexpr int ToggleTop = 108;
constexpr int ToggleStep = 21;
constexpr int PadTop = 108;
constexpr int PadIdTop = 134;
constexpr int DefaultPadNameTop = 180;
constexpr int DefaultPadIdTop = 198;
constexpr int InfoHeight = 15;
constexpr int SliderTop = 16;
constexpr int SliderHeight = 26;
constexpr int SliderOverhang = 6;
constexpr int SliderValueWidth = 36;
constexpr int DeadzoneLow = 1;
constexpr int DeadzoneHigh = 127;

struct Toggle {
    int id;
    const wchar_t* text;
    bool (EmulatorSettingsImpl::*get)() const;
    void (EmulatorSettingsImpl::*set)(bool, bool);
    bool fallback;
};

// GR2Launcher's controller check boxes, the settings they show and those settings' defaults.
constexpr std::array Toggles{
    Toggle{IdMotion, L"Enable Motion Controls", &EmulatorSettingsImpl::IsMotionControlsEnabled,
           &EmulatorSettingsImpl::SetMotionControlsEnabled, true},
    Toggle{IdGyroSwap, L"Swap Gyro Yaw/Roll (Handheld)", &EmulatorSettingsImpl::IsGyroSwapYawRoll,
           &EmulatorSettingsImpl::SetGyroSwapYawRoll, false},
    Toggle{IdGyroYaw, L"Invert Gyro Yaw", &EmulatorSettingsImpl::IsGyroInvertYaw,
           &EmulatorSettingsImpl::SetGyroInvertYaw, false},
    Toggle{IdGyroPitch, L"Invert Gyro Pitch", &EmulatorSettingsImpl::IsGyroInvertX,
           &EmulatorSettingsImpl::SetGyroInvertX, false},
    Toggle{IdGyroRoll, L"Invert Gyro Roll", &EmulatorSettingsImpl::IsGyroInvertRoll,
           &EmulatorSettingsImpl::SetGyroInvertRoll, false},
    Toggle{IdVibration, L"Enable Vibration", &EmulatorSettingsImpl::IsVibrationEnabled,
           &EmulatorSettingsImpl::SetVibrationEnabled, true},
    Toggle{IdBackgroundInput, L"Enable Background Controller Input",
           &EmulatorSettingsImpl::IsBackgroundControllerInput,
           &EmulatorSettingsImpl::SetBackgroundControllerInput, false},
    Toggle{IdKeyboard, L"Use Keyboard as Keyboard", &EmulatorSettingsImpl::IsKeyboardUsedAsKeyboard,
           &EmulatorSettingsImpl::SetKeyboardUsedAsKeyboard, false},
};

struct Slider {
    int label;
    int bar;
    int value;
    const wchar_t* text;
    int fallback;
    int top;
};

struct Stick {
    int group;
    const wchar_t* text;
    std::string_view name;
    RECT area;
    std::array<Slider, 2> sliders;
};

// The input config's "analog_deadzone = leftjoystick, min, max" lines, with GR2Launcher's labels
// and defaults.
constexpr std::array Sticks{
    Stick{
        IdLeftStickGroup,
        L"Left Stick Deadzone",
        "leftjoystick",
        {18, 288, 290, 396},
        {Slider{IdLeftMinLabel, IdLeftMin, IdLeftMinValue, L"Min Deadzone (def:2 max:127)", 2, 306},
         Slider{IdLeftMaxLabel, IdLeftMax, IdLeftMaxValue, L"Max Deadzone (def:127 max:127)", 127,
                348}}},
    Stick{IdRightStickGroup,
          L"Right Stick Deadzone",
          "rightjoystick",
          {300, 288, 577, 396},
          {Slider{IdRightMinLabel, IdRightMin, IdRightMinValue, L"Min Deadzone (def:2 max:127)", 2,
                  306},
           Slider{IdRightMaxLabel, IdRightMax, IdRightMaxValue, L"Max Deadzone (def:127 max:127)",
                  127, 348}}},
};

// The minimum and maximum deadzone of each stick.
using Deadzones = std::array<std::array<int, 2>, Sticks.size()>;

// The Mouse && Keyboard tab: each camera's mouse look speeds, in percent of the base speed.
constexpr int SpeedLow = 10;
constexpr int SpeedHigh = 300;
constexpr int PromptsTop = 226;
// The Controller tab shows the same setting below the sticks.
constexpr int PadPromptsTop = 402;
constexpr int MouseNoteTop = 280;
constexpr int TouchSwipeTop = 336;
constexpr int TouchSwipeNoteTop = 356;
constexpr RECT KeyBindingsButton{18, 396, 178, 419};

// The button prompts setting's values, in the order of the combo box.
constexpr std::array<std::pair<const wchar_t*, std::string_view>, 7> PromptChoices{{
    {L"Automatic", "auto"},
    {L"PlayStation", "playstation"},
    {L"Xbox", "xbox"},
    {L"Nintendo Switch", "switch"},
    {L"Steam Deck", "steamdeck"},
    {L"Steam Controller", "steamcontroller"},
    {L"Keyboard and Mouse", "keyboard"},
}};

struct Speed {
    int label;
    int bar;
    int value;
    const wchar_t* text;
    int top;
    u32 Settings::Mouse::* setting;
};

struct Camera {
    int group;
    const wchar_t* text;
    RECT area;
    std::array<Speed, 2> speeds;
};

constexpr std::array Cameras{
    Camera{IdFootGroup,
           L"On Foot Camera",
           {18, 88, 290, 212},
           {Speed{IdFootHorizontalLabel, IdFootHorizontal, IdFootHorizontalValue,
                  L"Horizontal Sensitivity", 106, &Settings::Mouse::foot_horizontal},
            Speed{IdFootVerticalLabel, IdFootVertical, IdFootVerticalValue, L"Vertical Sensitivity",
                  154, &Settings::Mouse::foot_vertical}}},
    Camera{IdShiftGroup,
           L"Gravity Shift Camera",
           {300, 88, 577, 212},
           {Speed{IdShiftHorizontalLabel, IdShiftHorizontal, IdShiftHorizontalValue,
                  L"Horizontal Sensitivity", 106, &Settings::Mouse::shift_horizontal},
            Speed{IdShiftVerticalLabel, IdShiftVertical, IdShiftVerticalValue,
                  L"Vertical Sensitivity", 154, &Settings::Mouse::shift_vertical}}},
};

struct Pad {
    std::wstring name;
    std::string guid;
};

struct Row {
    int label;
    int combo;
    const wchar_t* text;
    int top;
};

// Rows 29 to 30 pixels apart, the original's spacing.
constexpr std::array Rows{
    Row{IdMonitorLabel, IdMonitor, L"Monitor", 182},
    Row{IdResolutionLabel, IdResolution, L"Monitor Resolution", 211},
    Row{IdRefreshLabel, IdRefresh, L"Refresh Rate", 241},
    Row{IdVSyncLabel, IdVSync, L"VSync", 270},
    Row{IdFrameRateLabel, IdFrameRate, L"FPS Cap", 299},
    Row{IdAspectLabel, IdAspect, L"Aspect Ratio", 328},
    Row{IdRenderSizeLabel, IdRenderSize, L"Render Resolution", 357},
};

// In the order of Settings::VSync.
constexpr std::array VSyncNames{L"Off", L"Double Buffered", L"Triple Buffered"};

// The Graphics tab: the preset, then each pass's choice. Quality choices are in the order of
// Settings::Quality; switches put On first.
constexpr int PresetTop = 88;
constexpr int TextureStreamingTop = 302;
constexpr int GraphicsNoteTop = 345;
// The Texture Streaming choices after Auto, in MB. One at least the GPU's memory keeps within the
// driver's budget, as Auto does.
constexpr std::array<u32, 13> TextureStreamingSizes{1024,  1536,  2048,  3072,  4096,  6144, 8192,
                                                    10240, 11264, 12288, 16384, 24576, 32768};
constexpr std::array<const wchar_t*, 4> QualityChoices{L"PS4 Original", L"Medium", L"Low", L"Off"};
constexpr std::array<const wchar_t*, 3> FogChoices{L"PS4 Original", L"Medium", L"Low"};
constexpr std::array<const wchar_t*, 2> SwitchChoices{L"On", L"Off"};

struct GraphicsRow {
    int label;
    int combo;
    const wchar_t* text;
    int top;
    std::span<const wchar_t* const> choices;
};

constexpr std::array GraphicsRows{
    GraphicsRow{IdShadowsLabel, IdShadows, L"Shadows", 128, QualityChoices},
    GraphicsRow{IdFogLabel, IdFog, L"Volumetric Fog", 157, FogChoices},
    GraphicsRow{IdOcclusionLabel, IdOcclusion, L"Ambient Occlusion", 186, SwitchChoices},
    GraphicsRow{IdMotionBlurLabel, IdMotionBlur, L"Motion Blur", 215, SwitchChoices},
    GraphicsRow{IdAntialiasLabel, IdAntialias, L"Anti-Aliasing", 244, SwitchChoices},
    GraphicsRow{IdBloomLabel, IdBloom, L"Bloom", 273, SwitchChoices},
};

struct Monitor {
    std::wstring device;
    std::wstring label;
    RECT area;
    bool primary;
    int number;
};

struct Size {
    u32 width;
    u32 height;
    std::vector<u32> rates;
};

struct Modes {
    std::vector<Size> sizes;
    u32 current_width = 0;
    u32 current_height = 0;
    u32 current_rate = 0;
};

struct Dialog {
    const Art* art = nullptr;
    Settings::Values values;
    std::vector<Monitor> monitors;
    Modes modes;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    HFONT font = nullptr;
    HBITMAP banner = nullptr;
    HWND tab = nullptr;
    std::array<HWND, PageCount> pages{};
    std::vector<GamePatches::Extra> extras;
    // The user on controller port 1, who signs in to shadNet; null when there is none.
    User* player = nullptr;
    // A picture chosen and not saved yet, and the preview on the page.
    std::unique_ptr<Gdiplus::Bitmap> avatar;
    HBITMAP avatar_preview = nullptr;
    // SDL's gamepads, started when the Controller tab is first shown, and those connected then.
    bool gamepads_started = false;
    std::vector<Pad> pads;
    // The user picked the gamepad for this run.
    bool pad_picked = false;
    // The default gamepad's GUID as Ok saves it; empty for none.
    std::string default_pad;
    // The input config that holds the stick deadzones and the touch pad swipe switch, and their
    // values in it.
    std::filesystem::path input_config;
    Deadzones deadzones{};
    bool touch_swipe = false;
    // The keys of the game's functions as the Key Bindings dialog left them, to save on Ok.
    std::optional<KeyBindings> key_bindings;
    // The MB of each Texture Streaming choice; 0 for Auto.
    std::vector<u32> texture_streaming;
};

int Scaled(const Dialog& dialog, int value) {
    return MulDiv(value, static_cast<int>(dialog.dpi), USER_DEFAULT_SCREEN_DPI);
}

// Labels a monitor as the original does: its output number and its adapter.
BOOL CALLBACK AddMonitor(HMONITOR handle, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) {
        return TRUE;
    }
    Monitor monitor{
        info.szDevice, {}, info.rcMonitor, (info.dwFlags & MONITORINFOF_PRIMARY) != 0, 0};
    size_t digits = monitor.device.size();
    while (digits > 0 && std::iswdigit(monitor.device[digits - 1])) {
        --digits;
    }
    if (digits < monitor.device.size()) {
        monitor.number = std::stoi(monitor.device.substr(digits));
    }
    std::wstring adapter;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
        if (monitor.device == device.DeviceName) {
            adapter = device.DeviceString;
            break;
        }
        device.cb = sizeof(device);
    }
    monitor.label = std::to_wstring(monitor.number) + L" (" + adapter + L")";
    reinterpret_cast<std::vector<Monitor>*>(data)->push_back(std::move(monitor));
    return TRUE;
}

std::vector<Monitor> ListMonitors() {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &AddMonitor, reinterpret_cast<LPARAM>(&monitors));
    std::ranges::sort(monitors, {}, &Monitor::number);
    return monitors;
}

Modes ListModes(const std::wstring& device) {
    std::set<std::tuple<u32, u32, u32>> found;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD i = 0; EnumDisplaySettingsExW(device.c_str(), i, &mode, 0); ++i) {
        if (mode.dmBitsPerPel == 32 && (mode.dmDisplayFlags & DM_INTERLACED) == 0 &&
            mode.dmPelsWidth >= 800 && mode.dmPelsHeight >= 600 && mode.dmDisplayFrequency > 1) {
            found.emplace(mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
        }
    }
    Modes modes;
    mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode)) {
        modes.current_width = mode.dmPelsWidth;
        modes.current_height = mode.dmPelsHeight;
        modes.current_rate = mode.dmDisplayFrequency;
        found.emplace(mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency);
    }
    for (const auto& [width, height, rate] : found) {
        if (modes.sizes.empty() || modes.sizes.back().width != width ||
            modes.sizes.back().height != height) {
            modes.sizes.push_back({width, height, {}});
        }
        modes.sizes.back().rates.push_back(rate);
    }
    return modes;
}

HWND Item(const Dialog& dialog, int id) {
    for (const HWND page : dialog.pages) {
        if (const HWND item = GetDlgItem(page, id)) {
            return item;
        }
    }
    return nullptr;
}

std::wstring Wide(std::string_view text) {
    std::wstring wide(
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0),
        L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        static_cast<int>(wide.size()));
    return wide;
}

std::string Utf8(std::wstring_view text) {
    std::string narrow(WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr),
                       '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                        static_cast<int>(narrow.size()), nullptr, nullptr);
    return narrow;
}

std::wstring Text(HWND control) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    text.resize(
        static_cast<size_t>(GetWindowTextW(control, text.data(), static_cast<int>(text.size()))));
    return text;
}

void ShowAvatar(Dialog& dialog, Gdiplus::Bitmap* picture) {
    if (dialog.avatar_preview != nullptr) {
        DeleteObject(dialog.avatar_preview);
    }
    dialog.avatar_preview = Avatar::Preview(picture, Scaled(dialog, Avatar::Side));
    SendMessageW(Item(dialog, IdAvatar), STM_SETIMAGE, IMAGE_BITMAP,
                 reinterpret_cast<LPARAM>(dialog.avatar_preview));
}

// The Online tab shows player 1's shadNet account and profile picture.
void ShowOnline(Dialog& dialog) {
    UserManager& users = UserManagement;
    dialog.player = users.GetUserByPlayerIndex(1);
    if (dialog.player == nullptr && !users.GetUsers().user.empty()) {
        dialog.player = &users.GetUsers().user.front();
    }
    for (const int id : {IdOnlineIdLabel, IdOnlineId, IdPasswordLabel, IdPassword, IdAvatarLabel,
                         IdAvatar, IdAvatarChoose, IdAvatarNote}) {
        EnableWindow(Item(dialog, id), dialog.player != nullptr);
    }
    if (dialog.player == nullptr) {
        ShowAvatar(dialog, nullptr);
        return;
    }
    SetWindowTextW(Item(dialog, IdOnlineId), Wide(dialog.player->shadnet_npid).c_str());
    SetWindowTextW(Item(dialog, IdPassword), Wide(dialog.player->shadnet_password).c_str());
    const std::unique_ptr<Gdiplus::Bitmap> saved =
        Avatar::Make(Avatar::Path(dialog.player->user_id));
    ShowAvatar(dialog, saved.get());
}

void ChooseAvatar(HWND window, Dialog& dialog) {
    std::wstring file(MAX_PATH, L'\0');
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = window;
    open.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0";
    open.lpstrFile = file.data();
    open.nMaxFile = static_cast<DWORD>(file.size());
    open.lpstrTitle = L"Select Profile Picture";
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&open)) {
        return;
    }
    file.resize(std::wcslen(file.c_str()));
    std::unique_ptr<Gdiplus::Bitmap> picture = Avatar::Make(file);
    if (picture == nullptr) {
        MessageBoxW(window, L"The image could not be loaded.", L"Profile Picture",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    dialog.avatar = std::move(picture);
    ShowAvatar(dialog, dialog.avatar.get());
}

// Writes player 1's account to users.json and a newly chosen picture to the user's folder.
void SaveOnline(HWND window, const Dialog& dialog) {
    if (dialog.player == nullptr) {
        return;
    }
    dialog.player->shadnet_npid = Utf8(Text(Item(dialog, IdOnlineId)));
    dialog.player->shadnet_password = Utf8(Text(Item(dialog, IdPassword)));
    dialog.player->shadnet_enabled =
        !dialog.player->shadnet_npid.empty() && !dialog.player->shadnet_password.empty();
    UserSettings.Save();
    if (dialog.avatar != nullptr &&
        !Avatar::Save(*dialog.avatar, Avatar::Path(dialog.player->user_id))) {
        MessageBoxW(window, L"The profile picture could not be saved.", L"Profile Picture",
                    MB_OK | MB_ICONWARNING);
    }
}

void Check(const Dialog& dialog, int id, bool on) {
    SendMessageW(Item(dialog, id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

// A list view's check box is its state image: 1 clear, 2 checked.
void CheckRow(HWND list, int row, bool on) {
    LVITEMW item{};
    item.stateMask = LVIS_STATEIMAGEMASK;
    item.state = INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
    SendMessageW(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
}

bool RowChecked(HWND list, int row) {
    const LRESULT state =
        SendMessageW(list, LVM_GETITEMSTATE, static_cast<WPARAM>(row), LVIS_STATEIMAGEMASK);
    return (state & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
}

void AddText(HWND combo, const std::wstring& text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

int Selected(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

void Select(HWND combo, int index) {
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

bool Checked(const Dialog& dialog, int id) {
    return SendMessageW(Item(dialog, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// A config line without its comment and spaces, as the input config parser reads it.
std::string Compact(std::string_view line) {
    std::string compact(line.substr(0, line.find('#')));
    std::erase_if(compact, [](unsigned char c) { return std::isspace(c) != 0; });
    return compact;
}

std::string DeadzonePrefix(const Stick& stick) {
    return "analog_deadzone=" + std::string(stick.name) + ",";
}

// A stick without a deadzone line keeps the parser's 1 and 127.
Deadzones ReadDeadzones(const std::filesystem::path& file) {
    Deadzones deadzones;
    deadzones.fill({DeadzoneLow, DeadzoneHigh});
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        for (size_t i = 0; i < Sticks.size(); ++i) {
            const std::string prefix = DeadzonePrefix(Sticks[i]);
            if (!compact.starts_with(prefix)) {
                continue;
            }
            const char* const last = compact.data() + compact.size();
            int low = 0;
            int high = 0;
            const auto [comma, low_error] =
                std::from_chars(compact.data() + prefix.size(), last, low);
            if (low_error != std::errc{} || comma == last || *comma != ',') {
                continue;
            }
            const auto [end, high_error] = std::from_chars(comma + 1, last, high);
            if (high_error == std::errc{} && end == last) {
                deadzones[i] = {std::clamp(low, DeadzoneLow, DeadzoneHigh),
                                std::clamp(high, DeadzoneLow, DeadzoneHigh)};
            }
        }
    }
    return deadzones;
}

// A line of the input config to write: the start of the line it replaces, as Compact gives it,
// and the line.
struct ConfigLine {
    std::string prefix;
    std::string line;
};

// Rewrites the input config's lines that start with each prefix and adds the missing ones at its
// end.
bool SaveConfigLines(const std::filesystem::path& file, const std::vector<ConfigLine>& changes) {
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            lines.push_back(std::move(line));
        }
    }
    std::vector<bool> written(changes.size());
    for (std::string& line : lines) {
        const std::string compact = Compact(line);
        for (size_t i = 0; i < changes.size(); ++i) {
            if (compact.starts_with(changes[i].prefix)) {
                line = changes[i].line;
                written[i] = true;
            }
        }
    }
    for (size_t i = 0; i < changes.size(); ++i) {
        if (!written[i]) {
            lines.push_back(changes[i].line);
        }
    }
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& line : lines) {
        out << line << '\n';
    }
    return !out.fail();
}

std::vector<ConfigLine> DeadzoneLines(const Deadzones& deadzones) {
    std::vector<ConfigLine> lines;
    for (size_t i = 0; i < Sticks.size(); ++i) {
        lines.push_back(
            {DeadzonePrefix(Sticks[i]), "analog_deadzone = " + std::string(Sticks[i].name) + ", " +
                                            std::to_string(deadzones[i][0]) + ", " +
                                            std::to_string(deadzones[i][1])});
    }
    return lines;
}

constexpr std::string_view TouchSwipePrefix = "touchpad_swipe_enabled=";

// Whether the input config turns touch pad swipes on: its last touchpad_swipe_enabled line.
bool ReadTouchSwipe(const std::filesystem::path& file) {
    bool on = false;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        if (compact.starts_with(TouchSwipePrefix)) {
            const std::string_view value =
                std::string_view{compact}.substr(TouchSwipePrefix.size());
            on = value == "true" || value == "1";
        }
    }
    return on;
}

Deadzones DefaultDeadzones() {
    Deadzones deadzones{};
    for (size_t i = 0; i < Sticks.size(); ++i) {
        for (size_t j = 0; j < Sticks[i].sliders.size(); ++j) {
            deadzones[i][j] = Sticks[i].sliders[j].fallback;
        }
    }
    return deadzones;
}

void ShowDeadzones(const Dialog& dialog, const Deadzones& deadzones) {
    for (size_t i = 0; i < Sticks.size(); ++i) {
        for (size_t j = 0; j < Sticks[i].sliders.size(); ++j) {
            const Slider& slider = Sticks[i].sliders[j];
            SendMessageW(Item(dialog, slider.bar), TBM_SETPOS, TRUE, deadzones[i][j]);
            SetWindowTextW(Item(dialog, slider.value), std::to_wstring(deadzones[i][j]).c_str());
        }
    }
}

Deadzones ShownDeadzones(const Dialog& dialog) {
    Deadzones deadzones{};
    for (size_t i = 0; i < Sticks.size(); ++i) {
        for (size_t j = 0; j < Sticks[i].sliders.size(); ++j) {
            deadzones[i][j] = static_cast<int>(
                SendMessageW(Item(dialog, Sticks[i].sliders[j].bar), TBM_GETPOS, 0, 0));
        }
    }
    return deadzones;
}

std::wstring Percent(LRESULT value) {
    return std::to_wstring(value) + L"%";
}

// The number beside a slider follows it.
void ShowSliderValue(const Dialog& dialog, HWND bar) {
    const LRESULT value = SendMessageW(bar, TBM_GETPOS, 0, 0);
    for (const Stick& stick : Sticks) {
        for (const Slider& slider : stick.sliders) {
            if (slider.bar == GetDlgCtrlID(bar)) {
                SetWindowTextW(Item(dialog, slider.value), std::to_wstring(value).c_str());
            }
        }
    }
    for (const Camera& camera : Cameras) {
        for (const Speed& speed : camera.speeds) {
            if (speed.bar == GetDlgCtrlID(bar)) {
                SetWindowTextW(Item(dialog, speed.value), Percent(value).c_str());
            }
        }
    }
}

void ShowMouse(const Dialog& dialog, const Settings::Mouse& mouse) {
    const auto prompts = std::ranges::find(PromptChoices, std::string_view{mouse.prompts},
                                           &std::pair<const wchar_t*, std::string_view>::second);
    for (const int combo : {IdPrompts, IdPadPrompts}) {
        Select(Item(dialog, combo), prompts != PromptChoices.end()
                                        ? static_cast<int>(prompts - PromptChoices.begin())
                                        : 0);
    }
    for (const Camera& camera : Cameras) {
        for (const Speed& speed : camera.speeds) {
            const int value =
                std::clamp(static_cast<int>(mouse.*speed.setting), SpeedLow, SpeedHigh);
            SendMessageW(Item(dialog, speed.bar), TBM_SETPOS, TRUE, value);
            SetWindowTextW(Item(dialog, speed.value), Percent(value).c_str());
        }
    }
}

// The Automatic choice names the prompts it starts the game with, from the pads connected now.
void ShowAutomaticPrompts(const Dialog& dialog) {
    const std::string layout = PromptPacks::AutomaticLayout();
    const std::string_view setting = layout == "off" ? "playstation" : std::string_view{layout};
    const auto choice = std::ranges::find(PromptChoices, setting,
                                          &std::pair<const wchar_t*, std::string_view>::second);
    const std::wstring name = choice != PromptChoices.end() ? choice->first : L"PlayStation";
    const std::wstring text = std::wstring(PromptChoices[0].first) + L" (" + name + L")";
    for (const int id : {IdPrompts, IdPadPrompts}) {
        const HWND combo = Item(dialog, id);
        const int selected = Selected(combo);
        SendMessageW(combo, CB_DELETESTRING, 0, 0);
        SendMessageW(combo, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        Select(combo, selected);
    }
}

Settings::Mouse ReadMouse(const Dialog& dialog) {
    Settings::Mouse mouse = dialog.values.mouse;
    const int prompts = Selected(Item(dialog, IdPrompts));
    if (prompts >= 0 && prompts < static_cast<int>(PromptChoices.size())) {
        mouse.prompts = std::string(PromptChoices[prompts].second);
    }
    for (const Camera& camera : Cameras) {
        for (const Speed& speed : camera.speeds) {
            mouse.*speed.setting =
                static_cast<u32>(SendMessageW(Item(dialog, speed.bar), TBM_GETPOS, 0, 0));
        }
    }
    return mouse;
}

// The gamepads connected now, in SDL's order.
std::vector<Pad> ListPads() {
    std::vector<Pad> pads;
    SDL_UpdateJoysticks();
    int count = 0;
    SDL_JoystickID* const ids = SDL_GetGamepads(&count);
    if (ids == nullptr) {
        return pads;
    }
    for (int i = 0; i < count; ++i) {
        const char* const name = SDL_GetGamepadNameForID(ids[i]);
        pads.push_back({Wide(name != nullptr ? name : ""), Input::SelectedPad::GuidOf(ids[i])});
    }
    SDL_free(ids);
    return pads;
}

// GR2Launcher shows the last 16 digits of a GUID.
std::wstring PadId(std::string_view guid) {
    return L"ID: " + Wide(guid.substr(guid.size() > 16 ? guid.size() - 16 : 0));
}

void ShowActivePadId(const Dialog& dialog) {
    const int pad = Selected(Item(dialog, IdActivePad));
    const bool listed = pad >= 0 && pad < static_cast<int>(dialog.pads.size());
    SetWindowTextW(Item(dialog, IdActivePadId),
                   listed ? PadId(dialog.pads[pad].guid).c_str() : L"n/a");
}

void ShowDefaultPad(const Dialog& dialog) {
    const auto pad = std::ranges::find(dialog.pads, dialog.default_pad, &Pad::guid);
    const bool connected = !dialog.default_pad.empty() && pad != dialog.pads.end();
    const wchar_t* const missing =
        dialog.default_pad.empty() ? L"No default selected" : L"Default controller not connected";
    SetWindowTextW(Item(dialog, IdDefaultPadName), connected ? pad->name.c_str() : missing);
    SetWindowTextW(Item(dialog, IdDefaultPadId), connected ? PadId(pad->guid).c_str() : L"n/a");
}

// Lists the gamepads again. The one shown stays chosen; at first it is the one player 1 gets.
void ShowPads(Dialog& dialog) {
    if (!dialog.gamepads_started) {
        dialog.gamepads_started = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }
    const HWND combo = Item(dialog, IdActivePad);
    const int shown = Selected(combo);
    const std::string kept = shown >= 0 && shown < static_cast<int>(dialog.pads.size())
                                 ? dialog.pads[shown].guid
                                 : Input::SelectedPad::Guid();
    dialog.pads = dialog.gamepads_started ? ListPads() : std::vector<Pad>{};
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    if (dialog.pads.empty()) {
        AddText(combo, L"No gamepads detected");
    }
    for (size_t i = 0; i < dialog.pads.size(); ++i) {
        AddText(combo, std::to_wstring(i + 1) + L": " + dialog.pads[i].name);
    }
    const auto pad = std::ranges::find(dialog.pads, kept, &Pad::guid);
    Select(combo, pad != dialog.pads.end() ? static_cast<int>(pad - dialog.pads.begin()) : 0);
    EnableWindow(Item(dialog, IdSetDefaultPad), !dialog.pads.empty());
    ShowActivePadId(dialog);
    ShowDefaultPad(dialog);
}

// The Controller tab shows config.json's controller settings and the deadzones of the input
// config the game reads, and the Mouse && Keyboard tab that config's touch pad swipe switch. The
// gamepads are listed when the tab is shown.
void ShowController(Dialog& dialog) {
    for (const Toggle& toggle : Toggles) {
        Check(dialog, toggle.id, (EmulatorSettings.*toggle.get)());
    }
    dialog.input_config = Input::GetInputConfigFile(
        EmulatorSettings.IsUseUnifiedInputConfig() ? "default" : GameCheck::TitleId());
    dialog.deadzones = ReadDeadzones(dialog.input_config);
    ShowDeadzones(dialog, dialog.deadzones);
    dialog.touch_swipe = ReadTouchSwipe(dialog.input_config);
    Check(dialog, IdTouchSwipe, dialog.touch_swipe);
    dialog.default_pad = EmulatorSettings.GetDefaultControllerId();
}

// Saves the check boxes and the default gamepad to config.json and the deadzones and the touch pad
// swipe switch to the input config. The gamepad picked plays as player 1 in this run.
void SaveController(HWND window, const Dialog& dialog) {
    for (const Toggle& toggle : Toggles) {
        (EmulatorSettings.*toggle.set)(Checked(dialog, toggle.id), false);
    }
    EmulatorSettings.SetDefaultControllerId(dialog.default_pad);
    EmulatorSettings.Save();
    const int pad = Selected(Item(dialog, IdActivePad));
    if (dialog.pad_picked && pad >= 0 && pad < static_cast<int>(dialog.pads.size())) {
        Input::SelectedPad::Set(dialog.pads[pad].guid);
    }
    std::vector<ConfigLine> lines;
    const Deadzones deadzones = ShownDeadzones(dialog);
    if (deadzones != dialog.deadzones) {
        lines = DeadzoneLines(deadzones);
    }
    const bool touch_swipe = Checked(dialog, IdTouchSwipe);
    if (touch_swipe != dialog.touch_swipe) {
        lines.push_back({std::string(TouchSwipePrefix), std::string("touchpad_swipe_enabled = ") +
                                                            (touch_swipe ? "true" : "false")});
    }
    if (!lines.empty() && !SaveConfigLines(dialog.input_config, lines)) {
        MessageBoxW(window, L"The input config could not be saved.", L"Controller",
                    MB_OK | MB_ICONWARNING);
    }
}

void FillResolutions(Dialog& dialog, u32 width, u32 height) {
    const HWND combo = Item(dialog, IdResolution);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int chosen = -1;
    int current = -1;
    for (size_t i = 0; i < dialog.modes.sizes.size(); ++i) {
        const Size& size = dialog.modes.sizes[i];
        AddText(combo, std::to_wstring(size.width) + L" x " + std::to_wstring(size.height));
        if (size.width == width && size.height == height) {
            chosen = static_cast<int>(i);
        }
        if (size.width == dialog.modes.current_width &&
            size.height == dialog.modes.current_height) {
            current = static_cast<int>(i);
        }
    }
    Select(combo, chosen >= 0 ? chosen : current);
}

// A rate of 0 picks the monitor's current rate, or the highest when the size is not current.
void FillRates(Dialog& dialog, u32 rate) {
    const HWND combo = Item(dialog, IdRefresh);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const int size = Selected(Item(dialog, IdResolution));
    if (size < 0) {
        return;
    }
    const std::vector<u32>& rates = dialog.modes.sizes[size].rates;
    int chosen = -1;
    int current = -1;
    for (size_t i = 0; i < rates.size(); ++i) {
        AddText(combo, std::to_wstring(rates[i]) + L" Hz");
        if (rates[i] == rate) {
            chosen = static_cast<int>(i);
        }
        if (rates[i] == dialog.modes.current_rate) {
            current = static_cast<int>(i);
        }
    }
    Select(combo, chosen >= 0    ? chosen
                  : current >= 0 ? current
                                 : static_cast<int>(rates.size()) - 1);
}

u32 ShownRate(const Dialog& dialog) {
    const HWND combo = Item(dialog, IdRefresh);
    const int index = Selected(combo);
    if (index < 0) {
        return 0;
    }
    std::array<wchar_t, 32> text{};
    if (SendMessageW(combo, CB_GETLBTEXTLEN, index, 0) >= static_cast<LRESULT>(text.size())) {
        return 0;
    }
    SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
    return static_cast<u32>(std::wcstoul(text.data(), nullptr, 10));
}

void UpdateEnabled(const Dialog& dialog) {
    const bool fullscreen = Checked(dialog, IdFullscreen);
    const bool exclusive = fullscreen && Checked(dialog, IdExclusive);
    EnableWindow(Item(dialog, IdExclusive), fullscreen);
    EnableWindow(Item(dialog, IdResolutionLabel), !fullscreen || exclusive);
    EnableWindow(Item(dialog, IdResolution), !fullscreen || exclusive);
    EnableWindow(Item(dialog, IdRefreshLabel), exclusive);
    EnableWindow(Item(dialog, IdRefresh), exclusive);
}

void ShowGame(const Dialog& dialog, const Settings::Game& game) {
    const auto rate =
        std::ranges::find(GamePatches::FrameRates, game.frame_rate, &GamePatches::FrameRate::fps);
    Select(Item(dialog, IdFrameRate), rate != GamePatches::FrameRates.end()
                                          ? static_cast<int>(rate - GamePatches::FrameRates.begin())
                                          : 0);
    const auto aspect = std::ranges::find(GamePatches::Aspects, std::string_view{game.aspect},
                                          &GamePatches::Aspect::name);
    Select(Item(dialog, IdAspect), aspect != GamePatches::Aspects.end()
                                       ? static_cast<int>(aspect - GamePatches::Aspects.begin())
                                       : 0);
    const auto size =
        std::ranges::find(GamePatches::RenderSizes, std::string_view{game.render_size},
                          &GamePatches::RenderSize::name);
    const auto own_size = std::ranges::find(GamePatches::RenderSizes, std::string_view{"1080p"},
                                            &GamePatches::RenderSize::name);
    Select(Item(dialog, IdRenderSize),
           static_cast<int>((size != GamePatches::RenderSizes.end() ? size : own_size) -
                            GamePatches::RenderSizes.begin()));
    Check(dialog, IdDlc, game.dlc_entitlements);
    Check(dialog, IdHalloween, game.halloween_items);
    const HWND list = Item(dialog, IdExtras);
    for (size_t i = 0; i < dialog.extras.size(); ++i) {
        const auto chosen = game.extras.find(dialog.extras[i].name);
        CheckRow(list, static_cast<int>(i),
                 chosen != game.extras.end() ? chosen->second : dialog.extras[i].listed_on);
    }
}

// Extras keep only the choices that differ from the patch file's flags.
Settings::Game ReadGame(const Dialog& dialog) {
    Settings::Game game = dialog.values.game;
    const int rate = Selected(Item(dialog, IdFrameRate));
    if (rate >= 0 && rate < static_cast<int>(GamePatches::FrameRates.size())) {
        game.frame_rate = GamePatches::FrameRates[rate].fps;
    }
    const int aspect = Selected(Item(dialog, IdAspect));
    if (aspect >= 0 && aspect < static_cast<int>(GamePatches::Aspects.size())) {
        game.aspect = std::string(GamePatches::Aspects[aspect].name);
    }
    const int size = Selected(Item(dialog, IdRenderSize));
    if (size >= 0 && size < static_cast<int>(GamePatches::RenderSizes.size())) {
        game.render_size = std::string(GamePatches::RenderSizes[size].name);
    }
    game.dlc_entitlements = Checked(dialog, IdDlc);
    game.halloween_items = Checked(dialog, IdHalloween);
    game.extras.clear();
    const HWND list = Item(dialog, IdExtras);
    for (size_t i = 0; i < dialog.extras.size(); ++i) {
        const bool on = RowChecked(list, static_cast<int>(i));
        if (on != dialog.extras[i].listed_on) {
            game.extras.emplace(dialog.extras[i].name, on);
        }
    }
    return game;
}

void ShowGraphics(const Dialog& dialog, const Settings::Graphics& graphics) {
    Select(Item(dialog, IdShadows), static_cast<int>(graphics.shadows));
    Select(Item(dialog, IdFog),
           std::min(static_cast<int>(graphics.fog), static_cast<int>(FogChoices.size()) - 1));
    Select(Item(dialog, IdOcclusion), graphics.ambient_occlusion ? 0 : 1);
    Select(Item(dialog, IdMotionBlur), graphics.motion_blur ? 0 : 1);
    Select(Item(dialog, IdAntialias), graphics.antialiasing ? 0 : 1);
    Select(Item(dialog, IdBloom), graphics.bloom ? 0 : 1);
}

// The passes as shown, with the preset they and the Display tab's sizes match.
Settings::Graphics ReadGraphics(const Dialog& dialog) {
    Settings::Graphics graphics = dialog.values.graphics;
    const int shadows = Selected(Item(dialog, IdShadows));
    if (shadows >= 0 && shadows < static_cast<int>(QualityChoices.size())) {
        graphics.shadows = static_cast<Settings::Quality>(shadows);
    }
    const int fog = Selected(Item(dialog, IdFog));
    if (fog >= 0 && fog < static_cast<int>(FogChoices.size())) {
        graphics.fog = static_cast<Settings::Quality>(fog);
    }
    graphics.ambient_occlusion = Selected(Item(dialog, IdOcclusion)) != 1;
    graphics.motion_blur = Selected(Item(dialog, IdMotionBlur)) != 1;
    graphics.antialiasing = Selected(Item(dialog, IdAntialias)) != 1;
    graphics.bloom = Selected(Item(dialog, IdBloom)) != 1;
    const int texture_streaming = Selected(Item(dialog, IdTextureStreaming));
    if (texture_streaming >= 0 &&
        texture_streaming < static_cast<int>(dialog.texture_streaming.size())) {
        graphics.texture_streaming_mb = dialog.texture_streaming[texture_streaming];
    }
    graphics.preset = std::string(Settings::MatchPreset(ReadGame(dialog), graphics));
    return graphics;
}

// The preset the choices match, or Custom, the entry after the presets.
void ShowPreset(const Dialog& dialog) {
    const std::string preset = ReadGraphics(dialog).preset;
    const auto found =
        std::ranges::find(Settings::Presets, std::string_view{preset}, &Settings::Preset::key);
    Select(Item(dialog, IdPreset), static_cast<int>(found - Settings::Presets.begin()));
}

// Shows a preset's passes and, for the one that sets them, its aspect and render size and its
// handheld settings.
void ApplyPreset(const Dialog& dialog, int index) {
    if (index < 0 || index >= static_cast<int>(Settings::Presets.size())) {
        return;
    }
    const Settings::Preset& preset = Settings::Presets[index];
    ShowGraphics(dialog, Settings::FromPreset(preset));
    const auto aspect =
        std::ranges::find(GamePatches::Aspects, preset.aspect, &GamePatches::Aspect::name);
    if (aspect != GamePatches::Aspects.end()) {
        Select(Item(dialog, IdAspect), static_cast<int>(aspect - GamePatches::Aspects.begin()));
    }
    const auto size = std::ranges::find(GamePatches::RenderSizes, preset.render_size,
                                        &GamePatches::RenderSize::name);
    if (size != GamePatches::RenderSizes.end()) {
        Select(Item(dialog, IdRenderSize),
               static_cast<int>(size - GamePatches::RenderSizes.begin()));
    }
    if (preset.handheld) {
        Select(Item(dialog, IdVSync), static_cast<int>(Settings::VSync::Off));
        Check(dialog, IdGyroSwap, true);
        Check(dialog, IdGyroYaw, true);
    }
}

struct VideoCard {
    std::wstring name;
    // Its own memory and the RAM the system lends it.
    u64 bytes = 0;
    u64 shared = 0;
    // An integrated GPU, whose memory is all RAM.
    bool integrated = false;
};

// Whether the adapter's memory is the system's RAM.
bool Integrated(IDXGIAdapter1* adapter) {
    ID3D11Device* device = nullptr;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
        return false;
    }
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
    const bool unified = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,
                                                               &options, sizeof(options))) &&
                         options.UnifiedMemoryArchitecture;
    device->Release();
    return unified;
}

// The GPU GR2port draws with, as it picks one: the first that is not integrated, else the first;
// none when DXGI lists no hardware adapter.
VideoCard FindVideoCard() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return {};
    }
    std::optional<VideoCard> first;
    std::optional<VideoCard> discrete;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !discrete && SUCCEEDED(factory->EnumAdapters1(i, &adapter)); i++) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            const VideoCard card{desc.Description, desc.DedicatedVideoMemory,
                                 desc.SharedSystemMemory, Integrated(adapter)};
            if (!first) {
                first = card;
            }
            if (!card.integrated) {
                discrete = card;
            }
        }
        adapter->Release();
    }
    factory->Release();
    return discrete.value_or(first.value_or(VideoCard{}));
}

// The installed RAM, which includes what firmware keeps for an integrated GPU.
u64 InstalledRam() {
    ULONGLONG kilobytes = 0;
    if (GetPhysicallyInstalledSystemMemory(&kilobytes)) {
        return u64{kilobytes} << 10;
    }
    MEMORYSTATUSEX status{.dwLength = sizeof(MEMORYSTATUSEX)};
    return GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
}

// A size in MB as GB with at most one decimal.
std::wstring Gigabytes(u64 megabytes) {
    const u64 tenths = (megabytes * 10 + 512) / 1024;
    std::wstring text = std::to_wstring(tenths / 10);
    if (tenths % 10 != 0) {
        text += L"." + std::to_wstring(tenths % 10);
    }
    return text + L" GB";
}

// Auto, naming the GPU and its memory, or for an integrated GPU its share of RAM, then the
// sizes, then saved when it is none of them.
void FillTextureStreaming(Dialog& dialog, u32 saved) {
    const HWND combo = Item(dialog, IdTextureStreaming);
    const VideoCard card = FindVideoCard();
    std::wstring automatic = L"Auto";
    if (card.integrated) {
        const u64 share = std::min(card.bytes + card.shared, Settings::RamShare(InstalledRam()));
        automatic += L" (" + Gigabytes(share >> 20) + L" of RAM, " + card.name + L")";
    } else if (card.bytes != 0) {
        automatic += L" (" + Gigabytes(card.bytes >> 20) + L", " + card.name + L")";
    }
    dialog.texture_streaming = {0};
    AddText(combo, automatic);
    for (const u32 size : TextureStreamingSizes) {
        dialog.texture_streaming.push_back(size);
        AddText(combo, Gigabytes(size));
    }
    if (std::ranges::find(dialog.texture_streaming, saved) == dialog.texture_streaming.end()) {
        dialog.texture_streaming.push_back(saved);
        AddText(combo, Gigabytes(saved));
    }
}

void ShowTextureStreaming(const Dialog& dialog, u32 megabytes) {
    const auto found = std::ranges::find(dialog.texture_streaming, megabytes);
    Select(Item(dialog, IdTextureStreaming),
           found != dialog.texture_streaming.end()
               ? static_cast<int>(found - dialog.texture_streaming.begin())
               : 0);
}

void Show(Dialog& dialog, const Settings::Display& display) {
    Check(dialog, IdFullscreen, display.fullscreen);
    Check(dialog, IdExclusive, display.exclusive);
    int monitor = 0;
    for (size_t i = 0; i < dialog.monitors.size(); ++i) {
        const RECT& area = dialog.monitors[i].area;
        const bool chosen = display.monitor
                                ? PtInRect(&area, POINT{display.monitor->x, display.monitor->y})
                                : dialog.monitors[i].primary;
        if (chosen) {
            monitor = static_cast<int>(i);
        }
    }
    Select(Item(dialog, IdMonitor), monitor);
    dialog.modes = dialog.monitors.empty() ? Modes{} : ListModes(dialog.monitors[monitor].device);
    FillResolutions(dialog, display.width, display.height);
    FillRates(dialog, display.refresh_rate);
    Select(Item(dialog, IdVSync), static_cast<int>(display.vsync));
    UpdateEnabled(dialog);
}

// The primary monitor is stored as none, so it follows the primary monitor when that changes.
Settings::Display Read(const Dialog& dialog) {
    Settings::Display display = dialog.values.display;
    display.fullscreen = Checked(dialog, IdFullscreen);
    display.exclusive = Checked(dialog, IdExclusive);
    const int monitor = Selected(Item(dialog, IdMonitor));
    if (monitor >= 0 && monitor < static_cast<int>(dialog.monitors.size())) {
        const Monitor& chosen = dialog.monitors[monitor];
        display.monitor.reset();
        if (!chosen.primary) {
            display.monitor = Settings::Point{(chosen.area.left + chosen.area.right) / 2,
                                              (chosen.area.top + chosen.area.bottom) / 2};
        }
    }
    const int size = Selected(Item(dialog, IdResolution));
    if (size >= 0 && size < static_cast<int>(dialog.modes.sizes.size())) {
        display.width = dialog.modes.sizes[size].width;
        display.height = dialog.modes.sizes[size].height;
    }
    display.refresh_rate = ShownRate(dialog);
    const int vsync = Selected(Item(dialog, IdVSync));
    if (vsync >= 0 && vsync < static_cast<int>(VSyncNames.size())) {
        display.vsync = static_cast<Settings::VSync>(vsync);
    }
    return display;
}

// Kat's face from the splash, and the logo over the dark scarf beside her.
HBITMAP ComposeBanner(const Art& art, int width, int height, float scale) {
    return Art::Render(width, height, [&](Gdiplus::Graphics& graphics) {
        constexpr float Left = 860.0f;
        constexpr float Middle = 243.0f;
        const float full_width = static_cast<float>(width);
        const float full_height = static_cast<float>(height);
        const float part_width = art.SplashWidth() - Left;
        const float part_height = part_width * full_height / full_width;
        art.DrawSplash(graphics, {0.0f, 0.0f, full_width, full_height},
                       {Left, Middle - part_height / 2.0f, part_width, part_height});
        Art::Fade(graphics, {full_width * 0.62f, 0.0f, full_width * 0.38f, full_height}, 150);
        const float logo_height = 42.0f * scale;
        const float logo_width = logo_height * art.LogoAspect();
        art.DrawLogo(graphics,
                     {full_width - 10.0f * scale - logo_width,
                      (full_height - logo_height) / 2.0f + scale, logo_width, logo_height});
    });
}

// left and top are in the dialog's client area at 96 DPI; origin is the parent's place in it.
HWND MakeControl(const Dialog& dialog, HWND parent, POINT origin, const wchar_t* type,
                 const wchar_t* text, DWORD style, int id, int left, int top, int width,
                 int height) {
    const HWND control = CreateWindowExW(
        0, type, text, WS_CHILD | WS_VISIBLE | style, Scaled(dialog, left) - origin.x,
        Scaled(dialog, top) - origin.y, Scaled(dialog, width), Scaled(dialog, height), parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog.font), FALSE);
    return control;
}

HWND MakeButton(const Dialog& dialog, HWND parent, const wchar_t* text, DWORD style, int id,
                const RECT& area) {
    return MakeControl(dialog, parent, POINT{0, 0}, WC_BUTTONW, text, style | WS_TABSTOP, id,
                       area.left, area.top, area.right - area.left, area.bottom - area.top);
}

// Tab order follows the windows' order, so each control goes below the ones before it.
void KeepTabOrder(const std::vector<HWND>& controls) {
    for (const HWND control : controls) {
        SetWindowPos(control, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void Build(HWND window, Dialog& dialog) {
    dialog.dpi = GetDpiForWindow(window);
    dialog.font =
        CreateFontW(-MulDiv(8, static_cast<int>(dialog.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                    FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
    RECT frame{0, 0, Scaled(dialog, ClientWidth), Scaled(dialog, ClientHeight)};
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)), FALSE,
                             static_cast<DWORD>(GetWindowLongW(window, GWL_EXSTYLE)), dialog.dpi);
    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    RECT owner{};
    GetWindowRect(GetWindow(window, GW_OWNER), &owner);
    SetWindowPos(window, nullptr, (owner.left + owner.right - width) / 2,
                 (owner.top + owner.bottom - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    dialog.banner =
        ComposeBanner(*dialog.art, Scaled(dialog, ClientWidth), Scaled(dialog, BannerHeight),
                      static_cast<float>(dialog.dpi) / USER_DEFAULT_SCREEN_DPI);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const int tab_width = Scaled(dialog, TabArea.right - TabArea.left);
    const int tab_height = Scaled(dialog, TabArea.bottom - TabArea.top);
    const HWND tab =
        CreateWindowExW(WS_EX_CONTROLPARENT, WC_TABCONTROLW, L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                        Scaled(dialog, TabArea.left), Scaled(dialog, TabArea.top), tab_width,
                        tab_height, window, nullptr, instance, nullptr);
    SendMessageW(tab, WM_SETFONT, reinterpret_cast<WPARAM>(dialog.font), FALSE);
    dialog.tab = tab;
    for (size_t i = 0; i < PageCount; ++i) {
        std::wstring name = PageNames[i];
        TCITEMW tab_item{};
        tab_item.mask = TCIF_TEXT;
        tab_item.pszText = name.data();
        SendMessageW(tab, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&tab_item));
    }
    RECT page_area{0, 0, tab_width, tab_height};
    TabCtrl_AdjustRect(tab, FALSE, &page_area);
    for (size_t i = 0; i < PageCount; ++i) {
        dialog.pages[i] = CreateWindowExW(
            WS_EX_CONTROLPARENT, PageClass, L"", WS_CHILD | (i == DisplayPage ? WS_VISIBLE : 0),
            page_area.left, page_area.top, page_area.right - page_area.left,
            page_area.bottom - page_area.top, tab, nullptr, instance, nullptr);
    }
    const POINT page_origin{Scaled(dialog, TabArea.left) + page_area.left,
                            Scaled(dialog, TabArea.top) + page_area.top};

    const HWND display_page = dialog.pages[DisplayPage];
    std::vector<HWND> page_order;
    page_order.push_back(MakeControl(dialog, display_page, page_origin, WC_BUTTONW, L"Fullscreen",
                                     BS_AUTOCHECKBOX | WS_TABSTOP, IdFullscreen, CheckLeft,
                                     FullscreenTop, CheckWidth, CheckHeight));
    page_order.push_back(MakeControl(dialog, display_page, page_origin, WC_BUTTONW,
                                     L"Exclusive Fullscreen", BS_AUTOCHECKBOX | WS_TABSTOP,
                                     IdExclusive, CheckLeft, ExclusiveTop, CheckWidth,
                                     CheckHeight));
    for (const Row& row : Rows) {
        page_order.push_back(MakeControl(dialog, display_page, page_origin, WC_STATICW, row.text,
                                         SS_LEFT | SS_CENTERIMAGE, row.label, LabelLeft, row.top,
                                         LabelWidth, ComboHeight));
        page_order.push_back(MakeControl(dialog, display_page, page_origin, WC_COMBOBOXW, L"",
                                         CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, row.combo,
                                         ComboLeft, row.top, ComboWidth, ComboListHeight));
    }
    KeepTabOrder(page_order);

    const HWND graphics_page = dialog.pages[GraphicsPage];
    std::vector<HWND> graphics_order;
    graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_STATICW, L"Preset",
                                         SS_LEFT | SS_CENTERIMAGE, IdPresetLabel, LabelLeft,
                                         PresetTop, LabelWidth, ComboHeight));
    graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_COMBOBOXW, L"",
                                         CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdPreset,
                                         ComboLeft, PresetTop, ComboWidth, ComboListHeight));
    for (const GraphicsRow& row : GraphicsRows) {
        graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_STATICW,
                                             row.text, SS_LEFT | SS_CENTERIMAGE, row.label,
                                             LabelLeft, row.top, LabelWidth, ComboHeight));
        graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_COMBOBOXW, L"",
                                             CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, row.combo,
                                             ComboLeft, row.top, ComboWidth, ComboListHeight));
        for (const wchar_t* choice : row.choices) {
            AddText(Item(dialog, row.combo), choice);
        }
    }
    graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_STATICW,
                                         L"Texture Streaming", SS_LEFT | SS_CENTERIMAGE,
                                         IdTextureStreamingLabel, LabelLeft, TextureStreamingTop,
                                         LabelWidth, ComboHeight));
    graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_COMBOBOXW, L"",
                                         CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                         IdTextureStreaming, ComboLeft, TextureStreamingTop,
                                         ComboWidth, ComboListHeight));
    FillTextureStreaming(dialog, dialog.values.graphics.texture_streaming_mb);
    graphics_order.push_back(MakeControl(
        dialog, graphics_page, page_origin, WC_STATICW,
        L"Steam Deck also sets Aspect Ratio to 16:10, Render Resolution to 720p (1280 x 800) and "
        L"VSync to Off on the Display tab, and Swap Gyro Yaw/Roll and Invert Gyro Yaw on the "
        L"Controller tab. Changing a choice makes the preset Custom. With Texture Streaming "
        L"below the GPU's memory, textures keep fewer mip levels: first those not drawn for a "
        L"while, then the largest.",
        SS_LEFT, IdGraphicsNote, LabelLeft, GraphicsNoteTop, ExtrasList.right - LabelLeft,
        5 * InfoHeight));
    KeepTabOrder(graphics_order);
    for (const Settings::Preset& preset : Settings::Presets) {
        AddText(Item(dialog, IdPreset), Wide(preset.name));
    }
    AddText(Item(dialog, IdPreset), L"Custom");

    const HWND controller_page = dialog.pages[ControllerPage];
    const auto add = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id,
                         const RECT& area) {
        return MakeControl(dialog, controller_page, page_origin, type, text, style, id, area.left,
                           area.top, area.right - area.left, area.bottom - area.top);
    };
    std::vector<HWND> controller_order;
    controller_order.push_back(
        add(WC_BUTTONW, L"Controller", BS_GROUPBOX, IdControllerGroup, ControllerGroup));
    for (size_t i = 0; i < Toggles.size(); ++i) {
        const int top = ToggleTop + static_cast<int>(i) * ToggleStep;
        controller_order.push_back(add(WC_BUTTONW, Toggles[i].text, BS_AUTOCHECKBOX | WS_TABSTOP,
                                       Toggles[i].id,
                                       {ControllerGroup.left + GroupInset, top,
                                        ControllerGroup.right - GroupInset, top + CheckHeight}));
    }
    const int pad_left = ActivePadGroup.left + GroupInset;
    const int pad_right = ActivePadGroup.right - GroupInset;
    controller_order.push_back(
        add(WC_BUTTONW, L"Active Gamepad", BS_GROUPBOX, IdActivePadGroup, ActivePadGroup));
    controller_order.push_back(add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                   IdActivePad,
                                   {pad_left, PadTop, pad_right, PadTop + ComboListHeight}));
    controller_order.push_back(add(WC_STATICW, L"", SS_LEFT, IdActivePadId,
                                   {pad_left, PadIdTop, pad_right, PadIdTop + InfoHeight}));
    controller_order.push_back(
        add(WC_BUTTONW, L"Default Gamepad", BS_GROUPBOX, IdDefaultPadGroup, DefaultPadGroup));
    controller_order.push_back(
        add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, IdDefaultPadName,
            {pad_left, DefaultPadNameTop, pad_right, DefaultPadNameTop + InfoHeight}));
    controller_order.push_back(
        add(WC_STATICW, L"", SS_LEFT, IdDefaultPadId,
            {pad_left, DefaultPadIdTop, pad_right, DefaultPadIdTop + InfoHeight}));
    controller_order.push_back(add(WC_BUTTONW, L"Set Active Gamepad as Default",
                                   BS_PUSHBUTTON | WS_TABSTOP, IdSetDefaultPad,
                                   SetDefaultPadButton));
    controller_order.push_back(add(WC_BUTTONW, L"Remove Default Gamepad",
                                   BS_PUSHBUTTON | WS_TABSTOP, IdRemoveDefaultPad,
                                   RemoveDefaultPadButton));
    for (const Stick& stick : Sticks) {
        controller_order.push_back(
            add(WC_BUTTONW, stick.text, BS_GROUPBOX, stick.group, stick.area));
        const int left = stick.area.left + GroupInset;
        const int right = stick.area.right - GroupInset;
        for (const Slider& slider : stick.sliders) {
            controller_order.push_back(
                add(WC_STATICW, slider.text, SS_LEFT, slider.label,
                    {left, slider.top, right - SliderValueWidth, slider.top + InfoHeight}));
            controller_order.push_back(
                add(WC_STATICW, L"", SS_RIGHT, slider.value,
                    {right - SliderValueWidth, slider.top, right, slider.top + InfoHeight}));
            const int bar_top = slider.top + SliderTop;
            const HWND bar = add(
                TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, slider.bar,
                {left - SliderOverhang, bar_top, right + SliderOverhang, bar_top + SliderHeight});
            SendMessageW(bar, TBM_SETRANGE, FALSE, MAKELPARAM(DeadzoneLow, DeadzoneHigh));
            controller_order.push_back(bar);
        }
    }
    controller_order.push_back(
        add(WC_STATICW, L"Button Prompts", SS_LEFT | SS_CENTERIMAGE, IdPadPromptsLabel,
            {LabelLeft, PadPromptsTop, LabelLeft + LabelWidth, PadPromptsTop + ComboHeight}));
    controller_order.push_back(
        add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdPadPrompts,
            {ComboLeft, PadPromptsTop, ComboLeft + ComboWidth, PadPromptsTop + ComboListHeight}));
    KeepTabOrder(controller_order);

    const HWND mouse_page = dialog.pages[MousePage];
    const auto add_mouse = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id,
                               const RECT& area) {
        return MakeControl(dialog, mouse_page, page_origin, type, text, style, id, area.left,
                           area.top, area.right - area.left, area.bottom - area.top);
    };
    std::vector<HWND> mouse_order;
    for (const Camera& camera : Cameras) {
        mouse_order.push_back(
            add_mouse(WC_BUTTONW, camera.text, BS_GROUPBOX, camera.group, camera.area));
        const int left = camera.area.left + GroupInset;
        const int right = camera.area.right - GroupInset;
        for (const Speed& speed : camera.speeds) {
            mouse_order.push_back(
                add_mouse(WC_STATICW, speed.text, SS_LEFT, speed.label,
                          {left, speed.top, right - SliderValueWidth, speed.top + InfoHeight}));
            mouse_order.push_back(
                add_mouse(WC_STATICW, L"", SS_RIGHT, speed.value,
                          {right - SliderValueWidth, speed.top, right, speed.top + InfoHeight}));
            const int bar_top = speed.top + SliderTop;
            const HWND bar = add_mouse(
                TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, speed.bar,
                {left - SliderOverhang, bar_top, right + SliderOverhang, bar_top + SliderHeight});
            SendMessageW(bar, TBM_SETRANGE, FALSE, MAKELPARAM(SpeedLow, SpeedHigh));
            SendMessageW(bar, TBM_SETPAGESIZE, 0, 10);
            mouse_order.push_back(bar);
        }
    }
    mouse_order.push_back(
        add_mouse(WC_STATICW, L"Button Prompts", SS_LEFT | SS_CENTERIMAGE, IdPromptsLabel,
                  {LabelLeft, PromptsTop, LabelLeft + LabelWidth, PromptsTop + ComboHeight}));
    mouse_order.push_back(
        add_mouse(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdPrompts,
                  {ComboLeft, PromptsTop, ComboLeft + ComboWidth, PromptsTop + ComboListHeight}));
    for (const int combo : {IdPrompts, IdPadPrompts}) {
        for (const auto& [name, value] : PromptChoices) {
            AddText(Item(dialog, combo), name);
        }
    }
    mouse_order.push_back(add_mouse(
        WC_STATICW,
        L"The mouse turns the camera. The game's Camera Sensitivity option scales both. "
        L"Automatic follows the controller of player 1, or the keyboard when none is connected.",
        SS_LEFT, IdMouseNote,
        {LabelLeft, MouseNoteTop, ExtrasList.right, MouseNoteTop + 3 * InfoHeight}));
    mouse_order.push_back(add_mouse(
        WC_BUTTONW, L"Touchscreen to Touch Pad Swipes", BS_AUTOCHECKBOX | WS_TABSTOP, IdTouchSwipe,
        {LabelLeft, TouchSwipeTop, ExtrasList.right, TouchSwipeTop + CheckHeight}));
    mouse_order.push_back(add_mouse(
        WC_STATICW,
        L"Turns off mouse camera control. Touches and left-button drags become touch pad swipes, "
        L"and the cursor stays free.",
        SS_LEFT, IdTouchSwipeNote,
        {LabelLeft, TouchSwipeNoteTop, ExtrasList.right, TouchSwipeNoteTop + 2 * InfoHeight}));
    mouse_order.push_back(add_mouse(WC_BUTTONW, L"Key Bindings...", BS_PUSHBUTTON | WS_TABSTOP,
                                    IdKeyBindings, KeyBindingsButton));
    KeepTabOrder(mouse_order);

    const HWND dlc_page = dialog.pages[DlcPage];
    KeepTabOrder({
        MakeControl(dialog, dlc_page, page_origin, WC_BUTTONW, L"Enable DLC Entitlements",
                    BS_AUTOCHECKBOX | WS_TABSTOP, IdDlc, CheckLeft, FullscreenTop, CheckWidth,
                    CheckHeight),
        MakeControl(dialog, dlc_page, page_origin, WC_BUTTONW, L"Halloween Photo Items",
                    BS_AUTOCHECKBOX | WS_TABSTOP, IdHalloween, CheckLeft, ExclusiveTop, CheckWidth,
                    CheckHeight),
    });

    const HWND online_page = dialog.pages[OnlinePage];
    const std::wstring sign_up_url = Wide(GamePatches::ShadNetSignUp);
    const std::wstring sign_up =
        L"Sign up for shadNet: <a href=\"" + sign_up_url + L"\">" + sign_up_url + L"</a>";
    KeepTabOrder({
        MakeControl(dialog, online_page, page_origin, WC_LINK, sign_up.c_str(), WS_TABSTOP,
                    IdSignUp, LabelLeft, FullscreenTop, ExtrasList.right - LabelLeft, CheckHeight),
        MakeControl(dialog, online_page, page_origin, WC_STATICW, L"Online ID",
                    SS_LEFT | SS_CENTERIMAGE, IdOnlineIdLabel, LabelLeft, OnlineIdTop, LabelWidth,
                    ComboHeight),
        MakeControl(dialog, online_page, page_origin, WC_EDITW, L"",
                    WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, IdOnlineId, ComboLeft, OnlineIdTop,
                    ComboWidth, ComboHeight),
        MakeControl(dialog, online_page, page_origin, WC_STATICW, L"Password",
                    SS_LEFT | SS_CENTERIMAGE, IdPasswordLabel, LabelLeft, PasswordTop, LabelWidth,
                    ComboHeight),
        MakeControl(dialog, online_page, page_origin, WC_EDITW, L"",
                    WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, IdPassword, ComboLeft,
                    PasswordTop, ComboWidth, ComboHeight),
        MakeControl(dialog, online_page, page_origin, WC_STATICW, L"Profile Picture", SS_LEFT,
                    IdAvatarLabel, LabelLeft, AvatarTop, LabelWidth, CheckHeight),
        MakeControl(dialog, online_page, page_origin, WC_STATICW, L"", SS_BITMAP | SS_NOTIFY,
                    IdAvatar, ComboLeft, AvatarTop, Avatar::Side, Avatar::Side),
        MakeControl(dialog, online_page, page_origin, WC_BUTTONW, L"Choose Picture...",
                    BS_PUSHBUTTON | WS_TABSTOP, IdAvatarChoose, AvatarChooseButton.left,
                    AvatarChooseButton.top, AvatarChooseButton.right - AvatarChooseButton.left,
                    AvatarChooseButton.bottom - AvatarChooseButton.top),
        MakeControl(dialog, online_page, page_origin, WC_STATICW,
                    L"Gravity Rush 2 sends it to its server the next time you sign in to shadNet.",
                    SS_LEFT, IdAvatarNote, ComboLeft, AvatarTop + Avatar::Side + 10,
                    ExtrasList.right - ComboLeft, CheckHeight),
    });

    const std::wstring trophies = L"Sign in to <a href=\"" + Wide(GamePatches::ShadNetTrophies) +
                                  L"\">shadNet Trophies</a> to view your trophies.";
    MakeControl(dialog, dialog.pages[TrophiesPage], page_origin, WC_LINK, trophies.c_str(),
                WS_TABSTOP, IdTrophies, LabelLeft, FullscreenTop, ExtrasList.right - LabelLeft,
                CheckHeight);

    const HWND list =
        MakeControl(dialog, dialog.pages[ExtrasPage], page_origin, WC_LISTVIEWW, L"",
                    WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL,
                    IdExtras, ExtrasList.left, ExtrasList.top, ExtrasList.right - ExtrasList.left,
                    ExtrasList.bottom - ExtrasList.top);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = Scaled(dialog, ExtrasList.right - ExtrasList.left) -
                GetSystemMetricsForDpi(SM_CXVSCROLL, dialog.dpi) - Scaled(dialog, 4);
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    for (size_t i = 0; i < dialog.extras.size(); ++i) {
        std::wstring name = Wide(dialog.extras[i].name);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }

    KeepTabOrder({
        tab,
        MakeButton(dialog, window, L"Set To Default", BS_PUSHBUTTON, IdDefaults, DefaultsButton),
        MakeButton(dialog, window, L"Ok", BS_DEFPUSHBUTTON, IDOK, OkButton),
        MakeButton(dialog, window, L"Cancel", BS_PUSHBUTTON, IDCANCEL, CancelButton),
    });

    dialog.monitors = ListMonitors();
    for (const Monitor& monitor : dialog.monitors) {
        AddText(Item(dialog, IdMonitor), monitor.label);
    }
    for (const wchar_t* name : VSyncNames) {
        AddText(Item(dialog, IdVSync), name);
    }
    for (const GamePatches::FrameRate& rate : GamePatches::FrameRates) {
        AddText(Item(dialog, IdFrameRate), std::to_wstring(rate.fps) + L" FPS");
    }
    for (const GamePatches::Aspect& aspect : GamePatches::Aspects) {
        AddText(Item(dialog, IdAspect), Wide(aspect.name));
    }
    for (const GamePatches::RenderSize& size : GamePatches::RenderSizes) {
        AddText(Item(dialog, IdRenderSize), Wide(size.name));
    }
    Show(dialog, dialog.values.display);
    ShowGame(dialog, dialog.values.game);
    ShowGraphics(dialog, dialog.values.graphics);
    ShowTextureStreaming(dialog, dialog.values.graphics.texture_streaming_mb);
    ShowPreset(dialog);
    ShowController(dialog);
    ShowMouse(dialog, dialog.values.mouse);
    ShowOnline(dialog);
    SendMessageW(window, DM_SETDEFID, IDOK, 0);
    SetFocus(GetDlgItem(window, IDOK));
}

void Command(HWND window, Dialog& dialog, int id, int code) {
    switch (id) {
    case IdFullscreen:
    case IdExclusive:
        if (code == BN_CLICKED) {
            UpdateEnabled(dialog);
        }
        break;
    case IdMonitor:
        if (code == CBN_SELCHANGE) {
            const Settings::Display shown = Read(dialog);
            const int monitor = Selected(Item(dialog, IdMonitor));
            if (monitor >= 0 && monitor < static_cast<int>(dialog.monitors.size())) {
                dialog.modes = ListModes(dialog.monitors[monitor].device);
            }
            FillResolutions(dialog, shown.width, shown.height);
            FillRates(dialog, shown.refresh_rate);
        }
        break;
    case IdResolution:
        if (code == CBN_SELCHANGE) {
            FillRates(dialog, ShownRate(dialog));
        }
        break;
    case IdPreset:
        if (code == CBN_SELCHANGE) {
            ApplyPreset(dialog, Selected(Item(dialog, IdPreset)));
            ShowPreset(dialog);
        }
        break;
    case IdShadows:
    case IdFog:
    case IdOcclusion:
    case IdMotionBlur:
    case IdAntialias:
    case IdBloom:
    case IdAspect:
    case IdRenderSize:
        if (code == CBN_SELCHANGE) {
            ShowPreset(dialog);
        }
        break;
    case IdDefaults:
        if (code == BN_CLICKED) {
            Show(dialog, Settings::Display{});
            ShowGame(dialog, Settings::Game{});
            ShowGraphics(dialog, Settings::Graphics{});
            ShowTextureStreaming(dialog, Settings::Graphics{}.texture_streaming_mb);
            ShowPreset(dialog);
            for (const Toggle& toggle : Toggles) {
                Check(dialog, toggle.id, toggle.fallback);
            }
            ShowDeadzones(dialog, DefaultDeadzones());
            ShowMouse(dialog, Settings::Mouse{});
            Check(dialog, IdTouchSwipe, false);
            dialog.key_bindings = DefaultKeyBindings();
        }
        break;
    case IdPrompts:
    case IdPadPrompts:
        // Both tabs show the one setting.
        if (code == CBN_SELCHANGE) {
            Select(Item(dialog, id == IdPrompts ? IdPadPrompts : IdPrompts),
                   Selected(Item(dialog, id)));
        }
        break;
    case IdActivePad:
        if (code == CBN_SELCHANGE) {
            dialog.pad_picked = true;
            ShowActivePadId(dialog);
        }
        break;
    case IdSetDefaultPad:
        if (code == BN_CLICKED) {
            const int pad = Selected(Item(dialog, IdActivePad));
            if (pad >= 0 && pad < static_cast<int>(dialog.pads.size())) {
                dialog.default_pad = dialog.pads[pad].guid;
                ShowDefaultPad(dialog);
            }
        }
        break;
    case IdRemoveDefaultPad:
        if (code == BN_CLICKED) {
            dialog.default_pad.clear();
            ShowDefaultPad(dialog);
        }
        break;
    case IdKeyBindings:
        if (code == BN_CLICKED) {
            const KeyBindings keys =
                dialog.key_bindings ? *dialog.key_bindings : ReadKeyBindings(dialog.input_config);
            if (auto edited = EditKeyBindings(window, keys, dialog.values.mouse.control_type)) {
                dialog.key_bindings = std::move(edited);
            }
        }
        break;
    case IdAvatar:
    case IdAvatarChoose:
        if (code == BN_CLICKED || code == STN_CLICKED) {
            ChooseAvatar(window, dialog);
        }
        break;
    case IDOK:
        dialog.values.display = Read(dialog);
        dialog.values.game = ReadGame(dialog);
        dialog.values.graphics = ReadGraphics(dialog);
        dialog.values.mouse = ReadMouse(dialog);
        Settings::Set(dialog.values);
        SaveOnline(window, dialog);
        SaveController(window, dialog);
        if (dialog.key_bindings && !SaveKeyBindings(dialog.input_config, *dialog.key_bindings)) {
            MessageBoxW(window, L"The key bindings could not be saved.", L"Key Bindings",
                        MB_OK | MB_ICONWARNING);
        }
        EndDialog(window, IDOK);
        break;
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        break;
    default:
        break;
    }
}

void PaintBanner(HWND window, const Dialog& dialog) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    const HDC banner = CreateCompatibleDC(dc);
    const HGDIOBJ old_bitmap = SelectObject(banner, dialog.banner);
    BitBlt(dc, 0, 0, Scaled(dialog, ClientWidth), Scaled(dialog, BannerHeight), banner, 0, 0,
           SRCCOPY);
    SelectObject(banner, old_bitmap);
    DeleteDC(banner);
    EndPaint(window, &paint);
}

// The dialog and its page are white, as in the original.
INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lparam);
        Build(window, *reinterpret_cast<Dialog*>(lparam));
        return FALSE;
    }
    auto* const dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    if (dialog == nullptr) {
        return FALSE;
    }
    switch (message) {
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return reinterpret_cast<INT_PTR>(GetStockObject(WHITE_BRUSH));
    case WM_PAINT:
        PaintBanner(window, *dialog);
        return TRUE;
    case WM_COMMAND:
        Command(window, *dialog, LOWORD(wparam), HIWORD(wparam));
        return TRUE;
    case WM_HSCROLL:
        ShowSliderValue(*dialog, reinterpret_cast<HWND>(lparam));
        return TRUE;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->hwndFrom == dialog->tab && header->code == TCN_SELCHANGE) {
            const auto shown = static_cast<size_t>(SendMessageW(dialog->tab, TCM_GETCURSEL, 0, 0));
            if (shown == ControllerPage) {
                ShowPads(*dialog);
            }
            if (shown == ControllerPage || shown == MousePage) {
                ShowAutomaticPrompts(*dialog);
            }
            for (size_t i = 0; i < PageCount; ++i) {
                ShowWindow(dialog->pages[i], i == shown ? SW_SHOW : SW_HIDE);
            }
        }
        return TRUE;
    }
    default:
        return FALSE;
    }
}

// The page sits inside the tab control and hands its controls' messages to the dialog. The
// shadNet links open in the browser.
LRESULT CALLBACK PageProc(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
    case WM_HSCROLL:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetAncestor(page, GA_ROOT), message, wparam, lparam);
    case WM_NOTIFY: {
        const auto* link = reinterpret_cast<const NMLINK*>(lparam);
        if ((link->hdr.idFrom == IdSignUp || link->hdr.idFrom == IdTrophies) &&
            (link->hdr.code == NM_CLICK || link->hdr.code == NM_RETURN)) {
            ShellExecuteW(page, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        return DefWindowProcW(page, message, wparam, lparam);
    }
    default:
        return DefWindowProcW(page, message, wparam, lparam);
    }
}

} // namespace

void ShowSetup(HWND owner, const Art& art) {
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES |
                                                              ICC_LISTVIEW_CLASSES |
                                                              ICC_LINK_CLASS | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW page_class{};
    page_class.cbSize = sizeof(page_class);
    page_class.lpfnWndProc = &PageProc;
    page_class.hInstance = instance;
    page_class.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    page_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    page_class.lpszClassName = PageClass;
    RegisterClassExW(&page_class);

    // An empty dialog; Build makes the controls at the monitor's DPI.
    struct alignas(4) Template {
        DLGTEMPLATE header;
        WORD menu;
        WORD window_class;
        wchar_t title[24];
    };
    Template dialog_template{};
    dialog_template.header.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    wcscpy_s(dialog_template.title, L"Gravity Rush 2 - Setup");

    Dialog dialog;
    dialog.art = &art;
    dialog.values = Settings::Get();
    dialog.extras = GamePatches::Extras();
    DialogBoxIndirectParamW(instance, &dialog_template.header, owner, &DialogProc,
                            reinterpret_cast<LPARAM>(&dialog));
    DeleteObject(dialog.font);
    DeleteObject(dialog.banner);
    if (dialog.avatar_preview != nullptr) {
        DeleteObject(dialog.avatar_preview);
    }
    // The game starts SDL's gamepads again, with its own hints.
    if (dialog.gamepads_started) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
}

} // namespace Gr2Port::Launcher
