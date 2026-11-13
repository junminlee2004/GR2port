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
#include <unordered_map>
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

#include "common/path_util.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "core/game_check/game_check.h"
#include "core/game_check/game_patches.h"
#include "core/game_check/trophy_list.h"
#include "core/language/language.h"
#include "core/prompt_packs/prompt_packs.h"
#include "core/user_settings.h"
#include "input/input_handler.h"
#include "input/selected_pad.h"
#include "launcher/art.h"
#include "launcher/avatar.h"
#include "launcher/folder_picker.h"
#include "launcher/key_bindings.h"
#include "launcher/setup_dialog.h"
#include "launcher/trophy_viewer.h"

#include "graphics/gr2port/settings.h"

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
    IdUpscalerLabel,
    IdUpscaler,
    IdUpscaleQualityLabel,
    IdUpscaleQuality,
    IdLanguageLabel,
    IdLanguage,
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
    IdTrophyPopups,
    IdTrophyViewer,
    IdControllerGroup,
    IdMotion,
    IdGyroSwap,
    IdGyroYaw,
    IdGyroPitch,
    IdGyroRoll,
    IdVibration,
    IdGyroDeadzoneLabel,
    IdGyroDeadzone,
    IdGyroDeadzoneValue,
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
    IdPadBindings,
    IdExtras,
    IdPresetLabel,
    IdPreset,
    IdShadowsLabel,
    IdShadows,
    IdFogLabel,
    IdFog,
    IdFogShadowsLabel,
    IdFogShadows,
    IdOcclusionLabel,
    IdOcclusion,
    IdMotionBlurLabel,
    IdMotionBlur,
    IdAntialiasLabel,
    IdAntialias,
    IdBloomLabel,
    IdBloom,
    IdOutlinesLabel,
    IdOutlines,
    IdDecalsLabel,
    IdDecals,
    IdGlassLabel,
    IdGlass,
    IdParticlesLabel,
    IdParticles,
    IdAnisotropyLabel,
    IdAnisotropy,
    IdTextureStreamingLabel,
    IdTextureStreaming,
    IdFarDetailLabel,
    IdFarDetail,
    IdNearDetailLabel,
    IdNearDetail,
    IdCharacterDetailLabel,
    IdCharacterDetail,
    IdGraphicsNote,
    IdGameFolderGroup,
    IdGameFolder,
    IdGameFolderBrowse,
    IdUserFolderGroup,
    IdPortableFolder,
    IdPortablePath,
    IdGlobalFolder,
    IdGlobalPath,
    IdPathsNote,
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
    PathsPage,
    PageCount,
};

// Tab controls draw a single ampersand as an underline.
constexpr std::array<const wchar_t*, PageCount> PageNames{
    L"Display",  L"Graphics",         L"Controller", L"Mouse && Keyboard", L"DLC", L"Online",
    L"Trophies", L"Cheats && Extras", L"Paths"};

constexpr RECT ExtrasList{18, 88, 577, 424};
constexpr int OnlineIdTop = 121;
constexpr int PasswordTop = 150;
constexpr int AvatarTop = 192;
constexpr RECT AvatarChooseButton{338, 252, 448, 274};

// The Paths tab: the game's folder, and the user folder's two modes with the folder of each.
constexpr RECT GameFolderGroup{18, 88, 577, 146};
constexpr int GameFolderTop = 111;
constexpr RECT BrowseButton{477, 110, 565, 133};
constexpr RECT UserFolderGroup{18, 156, 577, 262};
constexpr int PortableTop = 177;
constexpr int GlobalTop = 219;
constexpr int FolderPathInset = 18;
constexpr int FolderPathDrop = 18;
constexpr int PathsNoteTop = 274;

// The Controller tab: GR2Launcher's controller settings in two columns of group boxes.
constexpr RECT ControllerGroup{18, 88, 290, 280};
constexpr RECT ActivePadGroup{300, 88, 577, 154};
constexpr RECT DefaultPadGroup{300, 160, 577, 280};
constexpr RECT SetDefaultPadButton{312, 220, 565, 243};
constexpr RECT RemoveDefaultPadButton{312, 249, 565, 272};
constexpr int GroupInset = 12;
constexpr int ToggleTop = 108;
constexpr int ToggleStep = 21;
constexpr int GyroDeadzoneTop = 234;
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
constexpr RECT PadBindingsButton{433, 402, 577, 424};
constexpr int TrophyPopupsTop = 118;
constexpr RECT TrophyViewerButton{18, 146, 178, 169};

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

// Rows 28 pixels apart, which fits ten below the check boxes.
constexpr std::array Rows{
    Row{IdMonitorLabel, IdMonitor, L"Monitor", 140},
    Row{IdResolutionLabel, IdResolution, L"Monitor Resolution", 168},
    Row{IdRefreshLabel, IdRefresh, L"Refresh Rate", 196},
    Row{IdVSyncLabel, IdVSync, L"VSync", 224},
    Row{IdFrameRateLabel, IdFrameRate, L"FPS Cap", 252},
    Row{IdAspectLabel, IdAspect, L"Aspect Ratio", 280},
    Row{IdRenderSizeLabel, IdRenderSize, L"Render Resolution", 308},
    Row{IdUpscalerLabel, IdUpscaler, L"Upscaler", 336},
    Row{IdUpscaleQualityLabel, IdUpscaleQuality, L"Upscaler Quality", 364},
    Row{IdLanguageLabel, IdLanguage, L"Language", 392},
};

// In the order of Settings::VSync.
constexpr std::array VSyncNames{L"Off", L"Double Buffered", L"Triple Buffered"};
// Indexed by Settings::Upscaler and Settings::UpscaleQuality.
constexpr std::array<const wchar_t*, 4> UpscalerChoices{L"Off", L"FSR 3.1", L"FSR 4", L"DLSS"};
constexpr std::array<const wchar_t*, 5> UpscaleQualityChoices{L"Native AA", L"Quality", L"Balanced",
                                                              L"Performance", L"Ultra Performance"};

// The Graphics tab: the preset across the top, each pass's choice in two columns of rows 28
// pixels apart, then the note. Quality choices are in the order of Settings::Quality; switches
// put On first.
constexpr int PresetTop = 88;
constexpr int PassTop = 118;
constexpr int PassStep = 28;
constexpr int GraphicsNoteTop = 348;
// The Texture Streaming list is wider than its combo box, to show the GPU's name.
constexpr int TextureStreamingListWidth = 280;
// The Texture Streaming choices after Auto, in MB. One at least the GPU's memory keeps within the
// driver's budget, as Auto does.
constexpr std::array<u32, 13> TextureStreamingSizes{1024,  1536,  2048,  3072,  4096,  6144, 8192,
                                                    10240, 11264, 12288, 16384, 24576, 32768};
constexpr std::array<const wchar_t*, 4> QualityChoices{L"PS4 Original", L"Medium", L"Low", L"Off"};
constexpr std::array<const wchar_t*, 3> FogChoices{L"PS4 Original", L"Medium", L"Low"};
constexpr std::array<const wchar_t*, 2> SwitchChoices{L"On", L"Off"};
// Ambient occlusion and motion blur offer three qualities each, in the order of their choices.
constexpr std::array<const wchar_t*, 3> OcclusionChoices{L"PS4 Original", L"Half Resolution",
                                                         L"Off"};
constexpr std::array OcclusionQualities{Settings::Quality::Original, Settings::Quality::Medium,
                                        Settings::Quality::Off};
constexpr std::array<const wchar_t*, 3> MotionBlurChoices{L"PS4 Original", L"Low", L"Off"};
constexpr std::array MotionBlurQualities{Settings::Quality::Original, Settings::Quality::Low,
                                         Settings::Quality::Off};
constexpr std::array<const wchar_t*, 2> ParticleChoices{L"Full Resolution", L"Half Resolution"};
// The most anisotropic filtering samples of each choice; 2 is the game's.
constexpr std::array<const wchar_t*, 5> AnisotropyChoices{L"Off", L"2x (PS4)", L"4x", L"8x",
                                                          L"16x"};
constexpr std::array<u32, 5> AnisotropyLevels{0, 2, 4, 8, 16};
// The near environment's and the characters' detail choices, in the order of
// Settings::DetailLevels, the far environment's in the order of Settings::FarDetailLevels, and the
// game's own among either.
constexpr std::array<const wchar_t*, 7> DetailChoices{L"400%", L"200%", L"150%", L"PS4 Original",
                                                      L"75%",  L"50%",  L"25%"};
constexpr std::array<const wchar_t*, 4> FarDetailChoices{L"400%", L"200%", L"150%",
                                                         L"PS4 Original"};
constexpr int OwnDetail = 3;

struct GraphicsColumn {
    int label_left;
    int label_width;
    int combo_left;
    int combo_width;
};

// Each column's labels fit their longest translation, 130 and 170 pixels.
constexpr GraphicsColumn LeftColumn{LabelLeft, 130, 152, 122};
constexpr GraphicsColumn RightColumn{280, 170, 454, 123};

struct GraphicsRow {
    int label;
    int combo;
    const wchar_t* text;
    GraphicsColumn column;
    int row;
    std::span<const wchar_t* const> choices;
};

// The left column, then the right; Texture Streaming's choices come from the GPU.
constexpr std::array GraphicsRows{
    GraphicsRow{IdShadowsLabel, IdShadows, L"Shadows", LeftColumn, 0, QualityChoices},
    GraphicsRow{IdFogLabel, IdFog, L"Volumetric Fog", LeftColumn, 1, FogChoices},
    GraphicsRow{IdFogShadowsLabel, IdFogShadows, L"Fog Shadows", LeftColumn, 2, SwitchChoices},
    GraphicsRow{IdOcclusionLabel, IdOcclusion, L"Ambient Occlusion", LeftColumn, 3,
                OcclusionChoices},
    GraphicsRow{IdMotionBlurLabel, IdMotionBlur, L"Motion Blur", LeftColumn, 4, MotionBlurChoices},
    GraphicsRow{IdAntialiasLabel, IdAntialias, L"Anti-Aliasing", LeftColumn, 5, SwitchChoices},
    GraphicsRow{IdBloomLabel, IdBloom, L"Bloom", LeftColumn, 6, SwitchChoices},
    GraphicsRow{IdCharacterDetailLabel, IdCharacterDetail, L"Character Detail", LeftColumn, 7,
                DetailChoices},
    GraphicsRow{IdOutlinesLabel, IdOutlines, L"Outlines", RightColumn, 0, SwitchChoices},
    GraphicsRow{IdDecalsLabel, IdDecals, L"Decals", RightColumn, 1, SwitchChoices},
    GraphicsRow{IdGlassLabel, IdGlass, L"Glass Refraction", RightColumn, 2, SwitchChoices},
    GraphicsRow{IdParticlesLabel, IdParticles, L"Particles", RightColumn, 3, ParticleChoices},
    GraphicsRow{IdAnisotropyLabel, IdAnisotropy, L"Anisotropic Filtering", RightColumn, 4,
                AnisotropyChoices},
    GraphicsRow{
        IdTextureStreamingLabel, IdTextureStreaming, L"Texture Streaming", RightColumn, 5, {}},
    GraphicsRow{IdFarDetailLabel, IdFarDetail, L"Far Environment LOD Quality", RightColumn, 6,
                FarDetailChoices},
    GraphicsRow{IdNearDetailLabel, IdNearDetail, L"Near Environment LOD Quality", RightColumn, 7,
                DetailChoices},
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
    // The pad buttons as the Button Bindings dialog left them, to save on Ok.
    std::optional<PadBindings> pad_bindings;
    // The MB of each Texture Streaming choice; 0 for Auto.
    std::vector<u32> texture_streaming;
    // Ok saved a change of the Paths tab, which the executable takes when it starts.
    bool restart = false;
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

// English text and the values its {N} markers name, shown in the language in use.
struct Phrase {
    std::wstring english;
    std::vector<std::wstring> values;

    Phrase(const wchar_t* text) : english{text} {}
    Phrase(std::wstring text) : english{std::move(text)} {}
    Phrase(std::wstring text, std::vector<std::wstring> named)
        : english{std::move(text)}, values{std::move(named)} {}

    std::wstring Shown() const {
        return Language::Text(english, values);
    }
};

// What each control and combo box item says in English, so a change of language can show it
// again. The dialog is modal: one at a time.
std::unordered_map<HWND, Phrase> g_texts;
std::unordered_map<HWND, std::vector<Phrase>> g_items;

void SetText(HWND control, Phrase text) {
    SetWindowTextW(control, text.Shown().c_str());
    g_texts.insert_or_assign(control, std::move(text));
}

void AddText(HWND combo, Phrase text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.Shown().c_str()));
    g_items[combo].push_back(std::move(text));
}

void ClearItems(HWND combo) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_items.erase(combo);
}

int Tell(HWND window, const wchar_t* text, const wchar_t* title, UINT style) {
    return MessageBoxW(window, Language::Text(text).c_str(), Language::Text(title).c_str(), style);
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
    // The filter's name, then its pattern, each ending in a null.
    std::wstring filter = Language::Text(L"Images") + L" (*.png;*.jpg;*.jpeg;*.bmp)";
    filter.push_back(L'\0');
    filter += L"*.png;*.jpg;*.jpeg;*.bmp";
    filter.push_back(L'\0');
    const std::wstring title = Language::Text(L"Select Profile Picture");
    OPENFILENAMEW open{};
    open.lStructSize = sizeof(open);
    open.hwndOwner = window;
    open.lpstrFilter = filter.c_str();
    open.lpstrFile = file.data();
    open.nMaxFile = static_cast<DWORD>(file.size());
    open.lpstrTitle = title.c_str();
    open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&open)) {
        return;
    }
    file.resize(std::wcslen(file.c_str()));
    std::unique_ptr<Gdiplus::Bitmap> picture = Avatar::Make(file);
    if (picture == nullptr) {
        Tell(window, L"The image could not be loaded.", L"Profile Picture", MB_OK | MB_ICONWARNING);
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
        Tell(window, L"The profile picture could not be saved.", L"Profile Picture",
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

int Selected(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

void Select(HWND combo, int index) {
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

bool Checked(const Dialog& dialog, int id) {
    return SendMessageW(Item(dialog, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void ShowPaths(const Dialog& dialog, const std::filesystem::path& game, bool global) {
    SetWindowTextW(Item(dialog, IdGameFolder),
                   (game.empty() ? Common::FS::GetExecutableDirectory() : game).c_str());
    Check(dialog, IdPortableFolder, !global);
    Check(dialog, IdGlobalFolder, global);
}

// Sets the game's folder, which SaveController saves, and asks for the user folder's mode.
// Returns whether either changed.
bool SavePaths(const Dialog& dialog) {
    std::filesystem::path game{Text(Item(dialog, IdGameFolder))};
    // No folder stands for the executable's.
    if (game == Common::FS::GetExecutableDirectory()) {
        game.clear();
    }
    const bool game_changed = game != EmulatorSettings.GetGameDirectory();
    if (game_changed) {
        EmulatorSettings.SetGameDirectory(game);
    }
    const auto mode = Checked(dialog, IdGlobalFolder) ? Common::FS::UserFolderMode::Global
                                                      : Common::FS::UserFolderMode::Portable;
    const bool mode_changed = mode != Common::FS::GetUserFolderMode();
    if (mode_changed) {
        Common::FS::RequestUserFolderMode(mode);
    }
    return game_changed || mode_changed;
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

std::wstring DegreesPerSecond(LRESULT value) {
    return std::to_wstring(value) + L"\xb0/s";
}

void ShowGyroDeadzone(const Dialog& dialog, int value) {
    SendMessageW(Item(dialog, IdGyroDeadzone), TBM_SETPOS, TRUE, value);
    SetWindowTextW(Item(dialog, IdGyroDeadzoneValue), DegreesPerSecond(value).c_str());
}

// The number beside a slider follows it.
void ShowSliderValue(const Dialog& dialog, HWND bar) {
    const LRESULT value = SendMessageW(bar, TBM_GETPOS, 0, 0);
    if (GetDlgCtrlID(bar) == IdGyroDeadzone) {
        SetWindowTextW(Item(dialog, IdGyroDeadzoneValue), DegreesPerSecond(value).c_str());
    }
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
    const Phrase text{L"Automatic ({0})", {Language::Text(name)}};
    for (const int id : {IdPrompts, IdPadPrompts}) {
        const HWND combo = Item(dialog, id);
        const int selected = Selected(combo);
        SendMessageW(combo, CB_DELETESTRING, 0, 0);
        SendMessageW(combo, CB_INSERTSTRING, 0, reinterpret_cast<LPARAM>(text.Shown().c_str()));
        Select(combo, selected);
        if (auto& items = g_items[combo]; !items.empty()) {
            items.front() = text;
        }
    }
}

// The prompts whose names the Button Bindings dialog gives the pad's buttons: the Controller
// tab's Button Prompts choice, PlayStation for the keyboard's.
std::string PadNamesLayout(const Dialog& dialog) {
    const int chosen = Selected(Item(dialog, IdPadPrompts));
    std::string layout = chosen >= 0 && chosen < static_cast<int>(PromptChoices.size())
                             ? std::string(PromptChoices[chosen].second)
                             : std::string("auto");
    if (layout == "auto") {
        layout = PromptPacks::AutomaticLayout();
    }
    return layout == "keyboard" ? std::string("playstation") : layout;
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
Phrase PadId(std::string_view guid) {
    return {L"ID: {0}", {Wide(guid.substr(guid.size() > 16 ? guid.size() - 16 : 0))}};
}

void ShowActivePadId(const Dialog& dialog) {
    const int pad = Selected(Item(dialog, IdActivePad));
    const bool listed = pad >= 0 && pad < static_cast<int>(dialog.pads.size());
    SetText(Item(dialog, IdActivePadId), listed ? PadId(dialog.pads[pad].guid) : Phrase{L"n/a"});
}

void ShowDefaultPad(const Dialog& dialog) {
    const auto pad = std::ranges::find(dialog.pads, dialog.default_pad, &Pad::guid);
    const bool connected = !dialog.default_pad.empty() && pad != dialog.pads.end();
    const Phrase missing =
        dialog.default_pad.empty() ? L"No default selected" : L"Default controller not connected";
    // A gamepad's name is shown as SDL gives it.
    SetText(Item(dialog, IdDefaultPadName), connected ? Phrase{L"{0}", {pad->name}} : missing);
    SetText(Item(dialog, IdDefaultPadId), connected ? PadId(pad->guid) : Phrase{L"n/a"});
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
    ClearItems(combo);
    if (dialog.pads.empty()) {
        AddText(combo, L"No gamepads detected");
    }
    for (size_t i = 0; i < dialog.pads.size(); ++i) {
        AddText(combo, Phrase{L"{0}: {1}", {std::to_wstring(i + 1), dialog.pads[i].name}});
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
    ShowGyroDeadzone(dialog, EmulatorSettings.GetGyroDeadzone());
    Check(dialog, IdTrophyPopups, !EmulatorSettings.IsTrophyPopupDisabled());
    dialog.input_config = Input::GetInputConfigFile(
        EmulatorSettings.IsUseUnifiedInputConfig() ? "default" : GameCheck::TitleId());
    dialog.deadzones = ReadDeadzones(dialog.input_config);
    ShowDeadzones(dialog, dialog.deadzones);
    dialog.touch_swipe = ReadTouchSwipe(dialog.input_config);
    Check(dialog, IdTouchSwipe, dialog.touch_swipe);
    dialog.default_pad = EmulatorSettings.GetDefaultControllerId();
}

// Saves the check boxes, the gyro deadzone and the default gamepad to config.json and the stick
// deadzones and the touch pad swipe switch to the input config. The gamepad picked plays as player
// 1 in this run.
void SaveController(HWND window, const Dialog& dialog) {
    for (const Toggle& toggle : Toggles) {
        (EmulatorSettings.*toggle.set)(Checked(dialog, toggle.id), false);
    }
    EmulatorSettings.SetGyroDeadzone(
        static_cast<int>(SendMessageW(Item(dialog, IdGyroDeadzone), TBM_GETPOS, 0, 0)), false);
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
        Tell(window, L"The input config could not be saved.", L"Controller",
             MB_OK | MB_ICONWARNING);
    }
}

void FillResolutions(Dialog& dialog, u32 width, u32 height) {
    const HWND combo = Item(dialog, IdResolution);
    ClearItems(combo);
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
    ClearItems(combo);
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

// The index of value in values, or fallback when it is none of them.
template <typename Value, size_t Count>
int IndexOf(const std::array<Value, Count>& values, Value value, int fallback) {
    const auto found = std::ranges::find(values, value);
    return found != values.end() ? static_cast<int>(found - values.begin()) : fallback;
}

// Sets value to the entry of values that the combo box shows, when it shows one.
template <typename Value, size_t Count>
void ReadChoice(HWND combo, const std::array<Value, Count>& values, Value& value) {
    const int index = Selected(combo);
    if (index >= 0 && index < static_cast<int>(Count)) {
        value = values[index];
    }
}

// A value the choices lack shows as the choice the renderer draws it as.
void ShowGraphics(const Dialog& dialog, const Settings::Graphics& graphics) {
    Select(Item(dialog, IdShadows), static_cast<int>(graphics.shadows));
    Select(Item(dialog, IdFog),
           std::min(static_cast<int>(graphics.fog), static_cast<int>(FogChoices.size()) - 1));
    Select(Item(dialog, IdFogShadows), graphics.fog_shadows ? 0 : 1);
    Select(Item(dialog, IdOcclusion), IndexOf(OcclusionQualities, graphics.ambient_occlusion, 1));
    Select(Item(dialog, IdMotionBlur), IndexOf(MotionBlurQualities, graphics.motion_blur, 0));
    Select(Item(dialog, IdAntialias), graphics.antialiasing ? 0 : 1);
    Select(Item(dialog, IdBloom), graphics.bloom ? 0 : 1);
    Select(Item(dialog, IdOutlines), graphics.outlines ? 0 : 1);
    Select(Item(dialog, IdDecals), graphics.decals ? 0 : 1);
    Select(Item(dialog, IdGlass), graphics.glass_refraction ? 0 : 1);
    Select(Item(dialog, IdParticles), graphics.half_resolution_particles ? 1 : 0);
    Select(Item(dialog, IdAnisotropy),
           IndexOf(AnisotropyLevels, graphics.anisotropic_filtering, 1));
    Select(Item(dialog, IdFarDetail),
           IndexOf(Settings::FarDetailLevels, graphics.far_environment_detail, OwnDetail));
    Select(Item(dialog, IdNearDetail),
           IndexOf(Settings::DetailLevels, graphics.near_environment_detail, OwnDetail));
    Select(Item(dialog, IdCharacterDetail),
           IndexOf(Settings::DetailLevels, graphics.character_detail, OwnDetail));
}

// The upscaler and its quality, which presets leave.
void ShowUpscaler(const Dialog& dialog, const Settings::Graphics& graphics) {
    Select(Item(dialog, IdUpscaler), static_cast<int>(graphics.upscaler));
    Select(Item(dialog, IdUpscaleQuality), static_cast<int>(graphics.upscale_quality));
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
    graphics.fog_shadows = Selected(Item(dialog, IdFogShadows)) != 1;
    ReadChoice(Item(dialog, IdOcclusion), OcclusionQualities, graphics.ambient_occlusion);
    ReadChoice(Item(dialog, IdMotionBlur), MotionBlurQualities, graphics.motion_blur);
    graphics.antialiasing = Selected(Item(dialog, IdAntialias)) != 1;
    graphics.bloom = Selected(Item(dialog, IdBloom)) != 1;
    graphics.outlines = Selected(Item(dialog, IdOutlines)) != 1;
    graphics.decals = Selected(Item(dialog, IdDecals)) != 1;
    graphics.glass_refraction = Selected(Item(dialog, IdGlass)) != 1;
    graphics.half_resolution_particles = Selected(Item(dialog, IdParticles)) == 1;
    ReadChoice(Item(dialog, IdAnisotropy), AnisotropyLevels, graphics.anisotropic_filtering);
    ReadChoice(Item(dialog, IdFarDetail), Settings::FarDetailLevels,
               graphics.far_environment_detail);
    ReadChoice(Item(dialog, IdNearDetail), Settings::DetailLevels,
               graphics.near_environment_detail);
    ReadChoice(Item(dialog, IdCharacterDetail), Settings::DetailLevels, graphics.character_detail);
    const int upscaler = Selected(Item(dialog, IdUpscaler));
    if (upscaler >= 0 && upscaler < static_cast<int>(UpscalerChoices.size())) {
        graphics.upscaler = static_cast<Settings::Upscaler>(upscaler);
    }
    const int upscale_quality = Selected(Item(dialog, IdUpscaleQuality));
    if (upscale_quality >= 0 && upscale_quality < static_cast<int>(UpscaleQualityChoices.size())) {
        graphics.upscale_quality = static_cast<Settings::UpscaleQuality>(upscale_quality);
    }
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

void ShowTextureStreaming(const Dialog& dialog, u32 megabytes);

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
    if (preset.texture_streaming_mb != 0) {
        ShowTextureStreaming(dialog, preset.texture_streaming_mb);
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
    Phrase automatic = L"Auto";
    if (card.integrated) {
        const u64 share = std::min(card.bytes + card.shared, Settings::RamShare(InstalledRam()));
        automatic = {L"Auto ({0} of RAM, {1})", {Gigabytes(share >> 20), card.name}};
    } else if (card.bytes != 0) {
        automatic = {L"Auto ({0}, {1})", {Gigabytes(card.bytes >> 20), card.name}};
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
                 const Phrase& text, DWORD style, int id, int left, int top, int width,
                 int height) {
    const HWND control = CreateWindowExW(
        0, type, text.Shown().c_str(), WS_CHILD | WS_VISIBLE | style,
        Scaled(dialog, left) - origin.x, Scaled(dialog, top) - origin.y, Scaled(dialog, width),
        Scaled(dialog, height), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(dialog.font), FALSE);
    if (!text.english.empty()) {
        g_texts.insert_or_assign(control, text);
    }
    return control;
}

HWND MakeButton(const Dialog& dialog, HWND parent, const Phrase& text, DWORD style, int id,
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

// Shows every text again in the language in use.
void Retranslate(const Dialog& dialog) {
    for (const auto& [control, text] : g_texts) {
        SetWindowTextW(control, text.Shown().c_str());
    }
    for (const auto& [combo, items] : g_items) {
        const int selected = Selected(combo);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (const Phrase& item : items) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.Shown().c_str()));
        }
        Select(combo, selected);
    }
    for (size_t i = 0; i < PageCount; ++i) {
        std::wstring name = Language::Text(PageNames[i]);
        TCITEMW tab_item{};
        tab_item.mask = TCIF_TEXT;
        tab_item.pszText = name.data();
        SendMessageW(dialog.tab, TCM_SETITEMW, i, reinterpret_cast<LPARAM>(&tab_item));
    }
    ShowAutomaticPrompts(dialog);
}

// Picks a console language in the Language list and shows the texts in it.
void ShowLanguage(const Dialog& dialog, int language) {
    const auto listed = Language::Listed();
    const auto found = std::ranges::find(listed, language);
    if (found == listed.end()) {
        return;
    }
    Select(Item(dialog, IdLanguage), static_cast<int>(found - listed.begin()));
    if (language != Language::Current()) {
        Language::Use(language);
        Retranslate(dialog);
    }
}

void Build(HWND window, Dialog& dialog) {
    g_texts.clear();
    g_items.clear();
    SetText(window, L"Gravity Rush 2 - Setup");
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
        std::wstring name = Language::Text(PageNames[i]);
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
    graphics_order.push_back(MakeControl(
        dialog, graphics_page, page_origin, WC_STATICW, L"Preset", SS_LEFT | SS_CENTERIMAGE,
        IdPresetLabel, LeftColumn.label_left, PresetTop, LeftColumn.label_width, ComboHeight));
    graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_COMBOBOXW, L"",
                                         CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IdPreset,
                                         LeftColumn.combo_left, PresetTop, ComboWidth,
                                         ComboListHeight));
    for (const GraphicsRow& row : GraphicsRows) {
        const int top = PassTop + row.row * PassStep;
        graphics_order.push_back(MakeControl(
            dialog, graphics_page, page_origin, WC_STATICW, row.text, SS_LEFT | SS_CENTERIMAGE,
            row.label, row.column.label_left, top, row.column.label_width, ComboHeight));
        graphics_order.push_back(MakeControl(dialog, graphics_page, page_origin, WC_COMBOBOXW, L"",
                                             CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, row.combo,
                                             row.column.combo_left, top, row.column.combo_width,
                                             ComboListHeight));
        for (const wchar_t* choice : row.choices) {
            AddText(Item(dialog, row.combo), choice);
        }
    }
    FillTextureStreaming(dialog, dialog.values.graphics.texture_streaming_mb);
    SendMessageW(Item(dialog, IdTextureStreaming), CB_SETDROPPEDWIDTH,
                 Scaled(dialog, TextureStreamingListWidth), 0);
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
    const int gyro_left = ControllerGroup.left + GroupInset;
    const int gyro_right = ControllerGroup.right - GroupInset;
    controller_order.push_back(add(
        WC_STATICW, L"Gyro Deadzone", SS_LEFT, IdGyroDeadzoneLabel,
        {gyro_left, GyroDeadzoneTop, gyro_right - SliderValueWidth, GyroDeadzoneTop + InfoHeight}));
    controller_order.push_back(add(WC_STATICW, L"", SS_RIGHT, IdGyroDeadzoneValue,
                                   {gyro_right - SliderValueWidth, GyroDeadzoneTop, gyro_right,
                                    GyroDeadzoneTop + InfoHeight}));
    const int gyro_bar_top = GyroDeadzoneTop + SliderTop;
    const HWND gyro_bar =
        add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, IdGyroDeadzone,
            {gyro_left - SliderOverhang, gyro_bar_top, gyro_right + SliderOverhang,
             gyro_bar_top + SliderHeight});
    SendMessageW(gyro_bar, TBM_SETRANGE, FALSE, MAKELPARAM(0, MaxGyroDeadzone));
    controller_order.push_back(gyro_bar);
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
    controller_order.push_back(add(WC_BUTTONW, L"Button Bindings...", BS_PUSHBUTTON | WS_TABSTOP,
                                   IdPadBindings, PadBindingsButton));
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
    const Phrase sign_up{L"Sign up for shadNet: <a href=\"{0}\">{0}</a>",
                         {Wide(GamePatches::ShadNetSignUp)}};
    KeepTabOrder({
        MakeControl(dialog, online_page, page_origin, WC_LINK, sign_up, WS_TABSTOP, IdSignUp,
                    LabelLeft, FullscreenTop, ExtrasList.right - LabelLeft, CheckHeight),
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

    const Phrase trophies{L"Sign in to <a href=\"{0}\">shadNet Trophies</a> to view your trophies.",
                          {Wide(GamePatches::ShadNetTrophies)}};
    MakeControl(dialog, dialog.pages[TrophiesPage], page_origin, WC_LINK, trophies, WS_TABSTOP,
                IdTrophies, LabelLeft, FullscreenTop, ExtrasList.right - LabelLeft, CheckHeight);
    // The game shows a pop-up when the player earns a trophy.
    MakeControl(dialog, dialog.pages[TrophiesPage], page_origin, WC_BUTTONW, L"Show Trophy Pop-ups",
                BS_AUTOCHECKBOX | WS_TABSTOP, IdTrophyPopups, LabelLeft, TrophyPopupsTop,
                CheckWidth, CheckHeight);
    MakeControl(dialog, dialog.pages[TrophiesPage], page_origin, WC_BUTTONW, L"View Trophies...",
                BS_PUSHBUTTON | WS_TABSTOP, IdTrophyViewer, TrophyViewerButton.left,
                TrophyViewerButton.top, TrophyViewerButton.right - TrophyViewerButton.left,
                TrophyViewerButton.bottom - TrophyViewerButton.top);

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

    const HWND paths_page = dialog.pages[PathsPage];
    const int paths_left = GameFolderGroup.left + GroupInset;
    const int paths_width = GameFolderGroup.right - GroupInset - paths_left;
    // Each mode's folder is under its name, cut in the middle when it is too long for the line.
    KeepTabOrder({
        MakeControl(dialog, paths_page, page_origin, WC_BUTTONW, L"Game Folder", BS_GROUPBOX,
                    IdGameFolderGroup, GameFolderGroup.left, GameFolderGroup.top,
                    GameFolderGroup.right - GameFolderGroup.left,
                    GameFolderGroup.bottom - GameFolderGroup.top),
        MakeControl(dialog, paths_page, page_origin, WC_EDITW, L"",
                    WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, IdGameFolder, paths_left,
                    GameFolderTop, BrowseButton.left - 8 - paths_left, ComboHeight),
        MakeControl(dialog, paths_page, page_origin, WC_BUTTONW, L"Browse...",
                    BS_PUSHBUTTON | WS_TABSTOP, IdGameFolderBrowse, BrowseButton.left,
                    BrowseButton.top, BrowseButton.right - BrowseButton.left,
                    BrowseButton.bottom - BrowseButton.top),
        MakeControl(dialog, paths_page, page_origin, WC_BUTTONW, L"User Data Folder", BS_GROUPBOX,
                    IdUserFolderGroup, UserFolderGroup.left, UserFolderGroup.top,
                    UserFolderGroup.right - UserFolderGroup.left,
                    UserFolderGroup.bottom - UserFolderGroup.top),
        MakeControl(dialog, paths_page, page_origin, WC_BUTTONW, L"Portable",
                    BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, IdPortableFolder, paths_left,
                    PortableTop, paths_width, CheckHeight),
        MakeControl(dialog, paths_page, page_origin, WC_STATICW, L"",
                    SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX, IdPortablePath,
                    paths_left + FolderPathInset, PortableTop + FolderPathDrop,
                    paths_width - FolderPathInset, InfoHeight),
        MakeControl(dialog, paths_page, page_origin, WC_BUTTONW, L"Global", BS_AUTORADIOBUTTON,
                    IdGlobalFolder, paths_left, GlobalTop, paths_width, CheckHeight),
        MakeControl(dialog, paths_page, page_origin, WC_STATICW, L"",
                    SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX, IdGlobalPath,
                    paths_left + FolderPathInset, GlobalTop + FolderPathDrop,
                    paths_width - FolderPathInset, InfoHeight),
        MakeControl(dialog, paths_page, page_origin, WC_STATICW,
                    L"The launcher restarts when Ok saves a change on this tab.",
                    SS_LEFT | WS_GROUP, IdPathsNote, LabelLeft, PathsNoteTop,
                    paths_left + paths_width - LabelLeft, 2 * InfoHeight),
    });
    SetWindowTextW(Item(dialog, IdPortablePath),
                   (Common::FS::GetExecutableDirectory() / Common::FS::PORTABLE_DIR).c_str());
    SetWindowTextW(Item(dialog, IdGlobalPath), Common::FS::GetGlobalUserFolder().c_str());
    ShowPaths(dialog, EmulatorSettings.GetGameDirectory(),
              Common::FS::GetUserFolderMode() == Common::FS::UserFolderMode::Global);

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
    for (const wchar_t* name : UpscalerChoices) {
        AddText(Item(dialog, IdUpscaler), name);
    }
    for (const wchar_t* name : UpscaleQualityChoices) {
        AddText(Item(dialog, IdUpscaleQuality), name);
    }
    // Each language is named in itself, whatever the language in use.
    for (const int language : Language::Listed()) {
        AddText(Item(dialog, IdLanguage), Phrase{L"{0}", {Language::WideName(language)}});
    }
    ShowLanguage(dialog, Language::Current());
    Show(dialog, dialog.values.display);
    ShowGame(dialog, dialog.values.game);
    ShowGraphics(dialog, dialog.values.graphics);
    ShowUpscaler(dialog, dialog.values.graphics);
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
    case IdLanguage:
        if (code == CBN_SELCHANGE) {
            const int chosen = Selected(Item(dialog, IdLanguage));
            const auto listed = Language::Listed();
            if (chosen >= 0 && chosen < static_cast<int>(listed.size())) {
                ShowLanguage(dialog, listed[chosen]);
            }
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
    case IdFogShadows:
    case IdOcclusion:
    case IdMotionBlur:
    case IdAntialias:
    case IdBloom:
    case IdOutlines:
    case IdDecals:
    case IdGlass:
    case IdParticles:
    case IdAnisotropy:
    case IdFarDetail:
    case IdNearDetail:
    case IdCharacterDetail:
    case IdAspect:
    case IdRenderSize:
    case IdTextureStreaming:
        if (code == CBN_SELCHANGE) {
            ShowPreset(dialog);
        }
        break;
    case IdDefaults:
        if (code == BN_CLICKED) {
            Show(dialog, Settings::Display{});
            ShowGame(dialog, Settings::Game{});
            ShowGraphics(dialog, Settings::Graphics{});
            ShowUpscaler(dialog, Settings::Graphics{});
            ShowTextureStreaming(dialog, Settings::Graphics{}.texture_streaming_mb);
            ShowPreset(dialog);
            for (const Toggle& toggle : Toggles) {
                Check(dialog, toggle.id, toggle.fallback);
            }
            ShowGyroDeadzone(dialog, InputSettings{}.gyro_deadzone.value);
            ShowDeadzones(dialog, DefaultDeadzones());
            ShowMouse(dialog, Settings::Mouse{});
            Check(dialog, IdTouchSwipe, false);
            Check(dialog, IdTrophyPopups, !GeneralSettings{}.trophy_popup_disabled.value);
            dialog.key_bindings = DefaultKeyBindings();
            dialog.pad_bindings = DefaultPadBindings();
            ShowLanguage(dialog, GeneralSettings{}.console_language.value);
            ShowPaths(dialog, {}, false);
        }
        break;
    case IdGameFolderBrowse:
        if (code == BN_CLICKED) {
            const std::optional<std::filesystem::path> folder = PickFolder(
                window, Language::Text(L"Choose the folder that holds your game and its update"));
            if (!folder) {
                break;
            }
            if (!GameCheck::HoldsGame(*folder)) {
                const std::array<std::wstring, 1> named{folder->wstring()};
                MessageBoxW(window,
                            Language::Text(L"No game or update was found in\n{0}", named).c_str(),
                            Language::Text(L"Game Folder").c_str(), MB_OK | MB_ICONWARNING);
                break;
            }
            SetWindowTextW(Item(dialog, IdGameFolder), folder->c_str());
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
    case IdPadBindings:
        if (code == BN_CLICKED) {
            const PadBindings buttons =
                dialog.pad_bindings ? *dialog.pad_bindings : ReadPadBindings(dialog.input_config);
            if (auto edited = EditPadBindings(window, buttons, dialog.values.mouse.control_type,
                                              PadNamesLayout(dialog))) {
                dialog.pad_bindings = std::move(edited);
            }
        }
        break;
    case IdAvatar:
    case IdAvatarChoose:
        if (code == BN_CLICKED || code == STN_CLICKED) {
            ChooseAvatar(window, dialog);
        }
        break;
    case IdTrophyViewer:
        if (code == BN_CLICKED) {
            Trophies::Extract();
            ShowTrophies(window);
        }
        break;
    case IDOK:
        dialog.values.display = Read(dialog);
        dialog.values.game = ReadGame(dialog);
        dialog.values.graphics = ReadGraphics(dialog);
        dialog.values.mouse = ReadMouse(dialog);
        Settings::Set(dialog.values);
        // The game starts in the language the launcher shows; SaveController saves it and the
        // trophy pop-ups' switch.
        EmulatorSettings.SetConsoleLanguage(Language::Current());
        EmulatorSettings.SetTrophyPopupDisabled(!Checked(dialog, IdTrophyPopups));
        dialog.restart = SavePaths(dialog);
        SaveOnline(window, dialog);
        SaveController(window, dialog);
        if (dialog.key_bindings && !SaveKeyBindings(dialog.input_config, *dialog.key_bindings)) {
            Tell(window, L"The key bindings could not be saved.", L"Key Bindings",
                 MB_OK | MB_ICONWARNING);
        }
        if (dialog.pad_bindings && !SavePadBindings(dialog.input_config, *dialog.pad_bindings)) {
            Tell(window, L"The button bindings could not be saved.", L"Button Bindings",
                 MB_OK | MB_ICONWARNING);
        }
        EndDialog(window, IDOK);
        break;
    case IDCANCEL:
        Language::Use(EmulatorSettings.GetConsoleLanguage());
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

bool ShowSetup(HWND owner, const Art& art) {
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
    return dialog.restart;
}

} // namespace Gr2Port::Launcher
