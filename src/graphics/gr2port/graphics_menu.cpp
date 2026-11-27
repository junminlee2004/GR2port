// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/game_check/game_patches.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/field_of_view.h"

#include "graphics/gr2port/graphics_menu.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/low_latency.h"
#include "graphics/gr2port/menu_kit.h"
#include "graphics/gr2port/page_sliders.h"
#include "graphics/gr2port/renderer.h"
#include "graphics/gr2port/screen_shake.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/ui.h"
#include "graphics/gr2port/ui_capture.h"
#include "graphics/gr2port/window.h"

namespace Gr2Port::GraphicsMenu {

namespace {

using namespace MenuKit;
using Settings::Quality;
using Settings::Values;

// u64 FUN_0130ebd0(system): places the System Menu's rows once they all loaded; true once they
// are placed.
constexpr Hooks::Entry PlaceRows{0x130ebd0, 6, Hooks::FramePrologue};
// void FUN_013124c0(page): the System Menu page's frame.
constexpr Hooks::Entry SystemFrame{0x13124c0, 6, Hooks::FramePrologue};
// void FUN_01310660(system, mode): slides the System Menu's frame and the shown category's rows.
constexpr Hooks::Entry SlideRows{0x1310660, 6, Hooks::FramePrologue};
// void FUN_0130d290(system): ends the System Menu's clips and rows.
constexpr Hooks::Entry EndSystem{0x130d290, 6, Hooks::FramePrologue};

// void FUN_01394df0(u64* handle): makes a clip handle and its clip.
constexpr VAddr MakeHandle = 0x1394df0;
// void FUN_013db4a0(bindings, slot, key) and FUN_013db1a0(float width, bindings, key): name the
// key a clip's text slot shows and the text's width, before the clip loads.
constexpr VAddr BindText = 0x13db4a0;
constexpr VAddr BindWidth = 0x13db1a0;
// void FUN_013e10f0(request, clip, asset, name): loads a clip. The request's callback runs when
// its last reference is dropped.
constexpr VAddr LoadClip = 0x13e10f0;
// void FUN_01716410(vector, count): reserves a vector of u32.
constexpr VAddr Reserve = 0x1716410;
// void FUN_01306fd0(row): finishes a System Menu row once its clip loaded.
constexpr VAddr RowLoaded = 0x1306fd0;
// void FUN_01307660(row): ends a row's clip handle and frees its node list; the row stays.
constexpr VAddr EndRow = 0x1307660;
// void FUN_01307790(row, mode): plays a row's slide: in, out, back to rest or the press.
constexpr VAddr AnimateRow = 0x1307790;
// void FUN_01307bc0(row, char shown) and FUN_01307c20(row, char selected)
constexpr VAddr ShowRow = 0x1307bc0;
constexpr VAddr SelectRow = 0x1307c20;
// void FUN_013df9d0(clip, canvas, 0, width, height): sets the canvas a clip draws on.
constexpr VAddr SetCanvas = 0x13df9d0;
// void FUN_01395160(handle): a clip handle's frame.
constexpr VAddr TickClip = 0x1395160;
// void FUN_013110e0(system, category): shows a category's rows and its header. Category 6 has no
// rows and gives the header of a sub-page.
constexpr VAddr ShowCategory = 0x13110e0;
// bool FUN_01310bd0(system, mode): whether the slide of the mode still plays.
constexpr VAddr Sliding = 0x1310bd0;
// void FUN_012fcb30(pause menu, page): asks the pause menu for a page; -1 closes the menu.
constexpr VAddr OpenPage = 0x12fcb30;
// void FUN_0130ddb0(system): hands the values of the game's own System pages to the game.
constexpr VAddr KeepValues = 0x130ddb0;
// entry FUN_013967a0(handle, node, index): the text a clip handle shows at a node.
// FUN_01390fb0(text, key) sets its key; the text looks its record up again at its next frame.
constexpr VAddr AttachText = 0x13967a0;
constexpr VAddr SetTextKey = 0x1390fb0;
// The interface's heap, and its posix_memalign and free.
constexpr VAddr Heap = 0x1cdf970;
constexpr VAddr HeapAlign = 0x17f56f0;
constexpr VAddr HeapFree = 0x17f56b0;
// The canvas the interface's clips draw on, 1920 x 1080.
constexpr VAddr Canvas = 0x1eeeb70;
constexpr u32 CanvasWidth = 1920;
constexpr u32 CanvasHeight = 1080;
// The object every System page shares, and the story: "crow" in Raven's Choice.
constexpr VAddr SystemObject = 0x1eee210;
constexpr VAddr Story = 0x1c80890;
// The game's names: the row clip, its text slots and its button prompts.
constexpr VAddr RowAsset = 0x18f3e7d;
constexpr VAddr Text01 = 0x18f7194;
constexpr VAddr Text02 = 0x18f719b;
constexpr VAddr Text03 = 0x18f3e76;
constexpr VAddr CircleSlot = 0x18f72d1;
constexpr VAddr CrossSlot = 0x18f72df;
// The class of a System Menu row.
constexpr VAddr RowClass = 0x1ae7d40;

// The System object: the clip handles of its layout and header, the System Menu's rows, every
// row, whether a page change keeps the object, the category it shows, and its load: 2 once its
// clips loaded, 3 once its rows are placed.
constexpr u64 SystemLayout = 0x00;
constexpr u64 SystemHeader = 0x08;
constexpr u64 SystemListFirst = 0x30;
constexpr u64 SystemListLast = 0x38;
constexpr u64 SystemRows = 0xf0;
constexpr u64 SystemKeep = 0x108;
constexpr u64 SystemCategory = 0x10c;
constexpr u64 SystemLoad = 0x130;
constexpr s32 SystemClipsLoaded = 2;
constexpr s32 MenuCategory = 0;
constexpr s32 PageCategory = 6;

// The System Menu page: its state, its wait for a save, and whether it runs.
constexpr u64 PageState = 0x08;
constexpr u64 PageSaveWait = 0x30;
constexpr u64 PageActive = 0x31;
constexpr s32 PageEnter = 0;
constexpr s32 PageInput = 2;
constexpr s32 PageClosing = 5;

// A System Menu row: its class, its kind (1, a button), its clip handle, its node list, its load
// (1 while the clip loads, 2 once loaded), whether it is selected, whether it has a caption, and
// its length from 0 to 1, which widens its text from 370 to 470 pixels.
constexpr size_t RowBytes = 0x48;
constexpr u64 RowKind = 0x08;
constexpr u64 RowHandle = 0x10;
constexpr u64 RowNodeList = 0x18;
constexpr u64 RowLoad = 0x38;
constexpr u64 RowSelected = 0x3d;
constexpr u64 RowCaptioned = 0x3f;
constexpr u64 RowLength = 0x44;
constexpr s32 RowButton = 1;
constexpr s32 RowLoading = 1;
constexpr s32 RowReady = 2;
constexpr u64 RowNodes = 6;
constexpr float TextWidth = 370.0f;
constexpr float CaptionWidth = 580.0f;
// A row's slides, as FUN_01307790 and FUN_01310660 take them.
constexpr u32 SlideIn = 0;
constexpr u32 SlideOut = 1;
constexpr u32 SlideRest = 2;
constexpr u32 Press = 3;
// Track 0 moves the row's control node: its slides, and its press in animation 6, which also
// plays animation 0 of track 4.
constexpr size_t ControlTrack = 0;
constexpr size_t PressAnimation = 6;
constexpr size_t PressSecondTrack = 4;

// A clip's text bindings.
constexpr u64 ClipBindings = 0x08;
// An attached text: the canvas pixel it draws from, from the left and down from the top, and
// whether its next draw shows it. Its clip handle's frame sets them from the text's node.
constexpr u64 TextCanvasX = 0xc0;
constexpr u64 TextCanvasY = 0xc4;
constexpr u64 TextShown = 0x167;
// A load request: its references, its callback and the callback's argument.
constexpr u64 RequestBytes = 0x18;
constexpr u64 RequestCallback = 0x08;
constexpr u64 RequestArgument = 0x10;

constexpr u32 DecideSound = 0x8bed3ae5;
constexpr u32 UpSound = 0xda16c583;
constexpr u32 DownSound = 0x33f3f50e;
constexpr u32 BackSound = 0x2b97730d;

// The System Menu has 6 rows, 5 in Raven's Choice, which lacks Photo Mode Settings.
constexpr size_t ListRows = 6;

// The page shows five rows, a caption over each value, 1.6 layout units apart from y 3.05 down.
// The System Menu's own places 1 and 2, at y 2.10 and 1.26, give the layout's direction.
constexpr size_t OnScreen = 5;
constexpr float FirstY = 3.05f;
constexpr float RowStep = -1.6f;
constexpr float PlaceY = 2.10f;
constexpr float PlaceStep = -0.84f;
// A row's first two attached texts are its value. The canvas pixels from the node a row's slide
// moves up to where the row's text slots draw, on its plate.
constexpr size_t ValueTexts = 2;
constexpr float ValueRise = 56.0f;

// A layout unit is 100 canvas pixels, from the canvas's middle, with y upwards.
constexpr float UnitPixels = 100.0f;
// The scroll bar stands beside the rows: its middle this many canvas pixels right of theirs on a
// 16:9 screen, from above the first row's caption to below the last row's plate. Its thumb is as
// long as the share of the settings the page shows, never shorter than MinThumb, and follows the
// page in ThumbTime.
constexpr float BarRight = 330.0f;
constexpr float BarWidth = 10.0f;
constexpr float BarAbove = 58.0f;
constexpr float BarBelow = 52.0f;
constexpr float MinThumb = 48.0f;
constexpr float ThumbTime = 0.06f;
// The message about the next start darkens the page under it.
constexpr float NoticeOpacity = 0.12f;

// The frames to wait for the rows' clips before the menu gives up on them.
constexpr u32 LoadFrames = 600;

constexpr u32 NvidiaVendor = 0x10de;

constexpr u32 Hash(std::string_view name) {
    u32 hash = 0x811c9dc5;
    for (const char character : name) {
        hash = (hash ^ static_cast<u8>(character)) * 0x01000193;
    }
    return hash;
}

constexpr u32 FirstPlace = Hash("loc_btn_1_1");
constexpr u32 SecondPlace = Hash("loc_btn_1_2");
// The place below the System Menu's rows, which the game leaves empty; Raven's Choice leaves the
// sixth empty.
constexpr u32 SeventhPlace = Hash("loc_btn_1_7");
constexpr u32 SixthPlace = Hash("loc_btn_1_6");
constexpr u32 HeaderText = Hash("loc_text_02");
static_assert(SeventhPlace == 0xd3d074c0 && HeaderText == 0x75aafd08);

constexpr char EntryKey[] = "gr2port_gfx_menu_1";
constexpr char EntrySelectedKey[] = "gr2port_gfx_menu_2";
constexpr char HeaderKey[] = "gr2port_gfx_header";
constexpr char NoticeKey[] = "gr2port_gfx_restart";
constexpr std::string_view LabelPrefix = "gr2port_gfx_l_";
// A value in the style the System Menu's own choices bind to a row's first text slot, which the
// selected look shows, and in the style of the second, which the plain look shows.
constexpr std::string_view SelectedValuePrefix = "gr2port_gfx_v1_";
constexpr std::string_view PlainValuePrefix = "gr2port_gfx_v2_";
// The page's rows bind empty texts to their clips' text slots, which the clip lays out only
// once; the texts that show are attached to the rows' nodes, and take new keys.
constexpr std::string_view BlankPrefix = "gr2port_gfx_blank_";

constexpr char Title[] = "Graphics Settings";
constexpr char Notice[] = "Some changes take effect the next time the game starts.";

using HandleFunction = void PS4_SYSV_ABI (*)(u64 handle);
using BindFunction = void PS4_SYSV_ABI (*)(u64 bindings, const char* slot, const char* key);
using WidthFunction = void PS4_SYSV_ABI (*)(float width, u64 bindings, const char* key);
using LoadFunction = void PS4_SYSV_ABI (*)(u64 request, u64 clip, const char* asset,
                                           const char* name);
using ReserveFunction = void PS4_SYSV_ABI (*)(u64 vector, u64 count);
using RowFunction = void PS4_SYSV_ABI (*)(u64 row);
using RowFlagFunction = void PS4_SYSV_ABI (*)(u64 row, u8 flag);
using RowModeFunction = void PS4_SYSV_ABI (*)(u64 row, u32 mode);
using CanvasFunction = void PS4_SYSV_ABI (*)(u64 clip, u64 canvas, u32 zero, u32 width, u32 height);
using SystemFunction = void PS4_SYSV_ABI (*)(u64 system);
using SystemModeFunction = void PS4_SYSV_ABI (*)(u64 system, u32 mode);
using SystemTestFunction = u8 PS4_SYSV_ABI (*)(u64 system, u32 mode);
using PlaceFunction = u64 PS4_SYSV_ABI (*)(u64 system);
using PageFunction = void PS4_SYSV_ABI (*)(u64 page);
using OpenPageFunction = void PS4_SYSV_ABI (*)(u64 menu, s32 page);
using AttachFunction = u64 PS4_SYSV_ABI (*)(u64 handle, u32 node, u32 index);
using SetKeyFunction = void PS4_SYSV_ABI (*)(u64 text, const char* key);
using AlignFunction = s32 PS4_SYSV_ABI (*)(u64 heap, u64* block, u64 alignment, u64 size);
using FreeFunction = void PS4_SYSV_ABI (*)(u64 heap, u64 block);

// A setting the page shows: its caption, the English names of its choices, the choice the values
// hold, and the values with another choice. The preset's last choice, Custom, shows only when the
// values match no preset.
struct Setting {
    const char* label;
    std::vector<std::string> (*names)(const Values& values);
    int (*get)(const Values& values);
    void (*set)(Values& values, int choice);
    bool custom_last = false;
};

using Names = std::vector<std::string>;

std::atomic<u32> g_vendor{0};

Names Choices(std::initializer_list<const char*> names) {
    return Names(names.begin(), names.end());
}

Names SwitchNames(const Values&) {
    return Choices({"On", "Off"});
}

Names DetailNames(const Values&) {
    return Choices({"400%", "200%", "150%", "PS4 Original", "75%", "50%", "25%"});
}

template <typename Value, size_t Count>
int IndexOf(const std::array<Value, Count>& levels, Value level, int fallback) {
    const auto found = std::ranges::find(levels, level);
    return found != levels.end() ? static_cast<int>(found - levels.begin()) : fallback;
}

// As the launcher's preset: its passes and detail, the shape, size, texture budget and shadow
// draw distance it sets, and no VSync for a handheld. The field of view, the screen shake, the
// upscaler, frame generation and low latency stay.
void ApplyPreset(Values& values, const Settings::Preset& preset) {
    Settings::Graphics graphics = Settings::FromPreset(preset);
    graphics.field_of_view = values.graphics.field_of_view;
    graphics.screen_shake = values.graphics.screen_shake;
    graphics.shadow_distance = preset.shadow_distance.value_or(values.graphics.shadow_distance);
    graphics.upscaler = values.graphics.upscaler;
    graphics.upscale_quality = values.graphics.upscale_quality;
    graphics.upscale_sharpening = values.graphics.upscale_sharpening;
    graphics.frame_generation = values.graphics.frame_generation;
    graphics.low_latency = values.graphics.low_latency;
    graphics.hud_gravity_meter = values.graphics.hud_gravity_meter;
    graphics.hud_health_bar = values.graphics.hud_health_bar;
    graphics.hud_crosshair = values.graphics.hud_crosshair;
    graphics.texture_streaming_mb =
        preset.texture_streaming_mb.value_or(values.graphics.texture_streaming_mb);
    graphics.pass_times = values.graphics.pass_times;
    values.graphics = graphics;
    if (!preset.aspect.empty()) {
        values.game.aspect = std::string(preset.aspect);
        values.game.render_size = std::string(preset.render_size);
    }
    if (preset.handheld) {
        values.display.vsync = Settings::VSync::Off;
    }
}

struct UpscalerChoice {
    Settings::Upscaler upscaler;
    const char* name;
};

// The upscalers the row lists: DLSS on NVIDIA GPUs, XeSS on Windows.
std::vector<UpscalerChoice> UpscalerChoices() {
    std::vector<UpscalerChoice> choices{{Settings::Upscaler::Off, "Off"},
                                        {Settings::Upscaler::Fsr3, "FSR 3.1"},
                                        {Settings::Upscaler::Fsr4, "FSR 4"}};
    if (g_vendor.load(std::memory_order_relaxed) == NvidiaVendor) {
        choices.push_back({Settings::Upscaler::Dlss, "DLSS"});
    }
#ifdef _WIN32
    choices.push_back({Settings::Upscaler::Xess, "XeSS"});
#endif
    return choices;
}

struct LowLatencyChoice {
    Settings::LowLatency mode;
    const char* name;
};

// Off, the drivers' ways the GPU has, and Universal.
std::vector<LowLatencyChoice> LowLatencyChoices() {
    std::vector<LowLatencyChoice> choices{{Settings::LowLatency::Off, "Off"}};
    if (LowLatency::Has(Settings::LowLatency::Reflex)) {
        choices.push_back({Settings::LowLatency::Reflex, "NVIDIA Reflex"});
    }
    if (LowLatency::Has(Settings::LowLatency::AntiLag2)) {
        choices.push_back({Settings::LowLatency::AntiLag2, "AMD Anti-Lag 2"});
    }
    choices.push_back({Settings::LowLatency::Universal, "Universal"});
    return choices;
}

// The texture budgets: Auto, the launcher's sizes, and the saved one when it is none of them.
std::vector<u32> TextureBudgets(const Values& values) {
    std::vector<u32> budgets{0,    1024,  1536,  2048,  3072,  4096,  6144,
                             8192, 10240, 11264, 12288, 16384, 24576, 32768};
    if (std::ranges::find(budgets, values.graphics.texture_streaming_mb) == budgets.end()) {
        budgets.push_back(values.graphics.texture_streaming_mb);
    }
    return budgets;
}

// A budget in gigabytes as the launcher writes it: one decimal when it has one.
std::string Gigabytes(u32 megabytes) {
    const u32 tenths = (megabytes * 10 + 512) / 1024;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        text += "." + std::to_string(tenths % 10);
    }
    return text + " GB";
}

// The monitor's sizes, and the saved one when it is none of them.
std::vector<Window::Size> Resolutions(const Values& values) {
    std::vector<Window::Size> sizes = Window::MonitorSizes();
    const auto saved = [&](const Window::Size& size) {
        return size.width == values.display.width && size.height == values.display.height;
    };
    if (std::ranges::none_of(sizes, saved)) {
        sizes.push_back({values.display.width, values.display.height});
    }
    return sizes;
}

constexpr std::array<Quality, 5> ShadowQualities{Quality::Max, Quality::Original, Quality::Medium,
                                                 Quality::Low, Quality::Off};
constexpr std::array<Quality, 3> OcclusionQualities{Quality::Original, Quality::Medium,
                                                    Quality::Off};
constexpr std::array<Quality, 3> BlurQualities{Quality::Original, Quality::Low, Quality::Off};
constexpr std::array<u32, 5> AnisotropyLevels{0, 2, 4, 8, 16};

const std::array<Setting, 36> Table{{
    {"Preset",
     [](const Values&) {
         Names names;
         for (const Settings::Preset& preset : Settings::Presets) {
             names.emplace_back(preset.name);
         }
         names.emplace_back("Custom");
         return names;
     },
     [](const Values& values) {
         const std::string_view key = Settings::MatchPreset(values.game, values.graphics);
         return static_cast<int>(std::ranges::find(Settings::Presets, key, &Settings::Preset::key) -
                                 Settings::Presets.begin());
     },
     [](Values& values, int choice) { ApplyPreset(values, Settings::Presets[choice]); }, true},
    {"Field of View",
     [](const Values&) {
         Names names;
         for (const u32 level : Settings::FieldOfViewLevels) {
             names.push_back(level == 100 ? std::string{"PS4 Original"}
                                          : std::to_string(level) + "%");
         }
         return names;
     },
     [](const Values& values) {
         return IndexOf(Settings::FieldOfViewLevels, values.graphics.field_of_view, 4);
     },
     [](Values& values, int choice) {
         values.graphics.field_of_view = Settings::FieldOfViewLevels[choice];
     }},
    {"Screen Shake",
     [](const Values&) {
         Names names;
         for (const u32 level : Settings::ScreenShakeLevels) {
             names.push_back(level == 0     ? std::string{"Off"}
                             : level == 100 ? std::string{"PS4 Original"}
                                            : std::to_string(level) + "%");
         }
         return names;
     },
     [](const Values& values) {
         return IndexOf(Settings::ScreenShakeLevels, values.graphics.screen_shake, 10);
     },
     [](Values& values, int choice) {
         values.graphics.screen_shake = Settings::ScreenShakeLevels[choice];
     }},
    {"HUD Gravity Meter", &SwitchNames,
     [](const Values& values) { return values.graphics.hud_gravity_meter ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.hud_gravity_meter = choice == 0; }},
    {"HUD Health Bar", &SwitchNames,
     [](const Values& values) { return values.graphics.hud_health_bar ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.hud_health_bar = choice == 0; }},
    {"HUD Crosshair", &SwitchNames,
     [](const Values& values) { return values.graphics.hud_crosshair ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.hud_crosshair = choice == 0; }},
    {"Shadows",
     [](const Values&) { return Choices({"Max", "PS4 Original", "Medium", "Low", "Off"}); },
     [](const Values& values) { return IndexOf(ShadowQualities, values.graphics.shadows, 1); },
     [](Values& values, int choice) { values.graphics.shadows = ShadowQualities[choice]; }},
    {"Shadow Draw Distance",
     [](const Values&) {
         Names names;
         for (const u32 level : Settings::ShadowDistanceLevels) {
             names.push_back(level == 100 ? std::string{"PS4 Original"}
                                          : std::to_string(level) + "%");
         }
         return names;
     },
     [](const Values& values) {
         return IndexOf(Settings::ShadowDistanceLevels, values.graphics.shadow_distance, 2);
     },
     [](Values& values, int choice) {
         values.graphics.shadow_distance = Settings::ShadowDistanceLevels[choice];
     }},
    {"Volumetric Fog", [](const Values&) { return Choices({"PS4 Original", "Medium", "Low"}); },
     [](const Values& values) { return std::min(static_cast<int>(values.graphics.fog), 2); },
     [](Values& values, int choice) { values.graphics.fog = static_cast<Quality>(choice); }},
    {"Fog Shadows", &SwitchNames,
     [](const Values& values) { return values.graphics.fog_shadows ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.fog_shadows = choice == 0; }},
    {"Ambient Occlusion",
     [](const Values&) { return Choices({"PS4 Original", "Half Resolution", "Off"}); },
     [](const Values& values) {
         const Quality quality = values.graphics.ambient_occlusion;
         return quality == Quality::Original ? 0 : quality == Quality::Off ? 2 : 1;
     },
     [](Values& values, int choice) {
         values.graphics.ambient_occlusion = OcclusionQualities[choice];
     }},
    {"Motion Blur", [](const Values&) { return Choices({"PS4 Original", "Low", "Off"}); },
     [](const Values& values) {
         const Quality quality = values.graphics.motion_blur;
         return quality == Quality::Original ? 0 : quality == Quality::Off ? 2 : 1;
     },
     [](Values& values, int choice) { values.graphics.motion_blur = BlurQualities[choice]; }},
    {"Anti-Aliasing", &SwitchNames,
     [](const Values& values) { return values.graphics.antialiasing ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.antialiasing = choice == 0; }},
    {"Bloom", &SwitchNames, [](const Values& values) { return values.graphics.bloom ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.bloom = choice == 0; }},
    {"Outlines", &SwitchNames,
     [](const Values& values) { return values.graphics.outlines ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.outlines = choice == 0; }},
    {"Decals", &SwitchNames, [](const Values& values) { return values.graphics.decals ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.decals = choice == 0; }},
    {"Glass Refraction", &SwitchNames,
     [](const Values& values) { return values.graphics.glass_refraction ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.glass_refraction = choice == 0; }},
    {"Particles", [](const Values&) { return Choices({"Full Resolution", "Half Resolution"}); },
     [](const Values& values) { return values.graphics.half_resolution_particles ? 1 : 0; },
     [](Values& values, int choice) { values.graphics.half_resolution_particles = choice == 1; }},
    {"Smoke and Dust", &SwitchNames,
     [](const Values& values) { return values.graphics.smoke_and_dust ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.smoke_and_dust = choice == 0; }},
    {"Distortion", &SwitchNames,
     [](const Values& values) { return values.graphics.distortion ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.distortion = choice == 0; }},
    {"Anisotropic Filtering",
     [](const Values&) { return Choices({"Off", "2x (PS4)", "4x", "8x", "16x"}); },
     [](const Values& values) {
         return IndexOf(AnisotropyLevels, values.graphics.anisotropic_filtering, 1);
     },
     [](Values& values, int choice) {
         values.graphics.anisotropic_filtering = AnisotropyLevels[choice];
     }},
    {"Texture Streaming",
     [](const Values& values) {
         Names names;
         for (const u32 budget : TextureBudgets(values)) {
             names.push_back(budget == 0 ? std::string{"Auto"} : Gigabytes(budget));
         }
         return names;
     },
     [](const Values& values) {
         const std::vector<u32> budgets = TextureBudgets(values);
         return static_cast<int>(std::ranges::find(budgets, values.graphics.texture_streaming_mb) -
                                 budgets.begin());
     },
     [](Values& values, int choice) {
         values.graphics.texture_streaming_mb = TextureBudgets(values)[choice];
     }},
    {"Character Detail", &DetailNames,
     [](const Values& values) {
         return IndexOf(Settings::DetailLevels, values.graphics.character_detail, 3);
     },
     [](Values& values, int choice) {
         values.graphics.character_detail = Settings::DetailLevels[choice];
     }},
    {"Near Environment LOD Quality", &DetailNames,
     [](const Values& values) {
         return IndexOf(Settings::DetailLevels, values.graphics.near_environment_detail, 3);
     },
     [](Values& values, int choice) {
         values.graphics.near_environment_detail = Settings::DetailLevels[choice];
     }},
    {"Far Environment LOD Quality",
     [](const Values&) { return Choices({"400%", "200%", "150%", "PS4 Original"}); },
     [](const Values& values) {
         return IndexOf(Settings::FarDetailLevels, values.graphics.far_environment_detail, 3);
     },
     [](Values& values, int choice) {
         values.graphics.far_environment_detail = Settings::FarDetailLevels[choice];
     }},
    {"Upscaler",
     [](const Values&) {
         Names names;
         for (const UpscalerChoice& choice : UpscalerChoices()) {
             names.emplace_back(choice.name);
         }
         return names;
     },
     [](const Values& values) {
         const std::vector<UpscalerChoice> choices = UpscalerChoices();
         const auto found =
             std::ranges::find(choices, values.graphics.upscaler, &UpscalerChoice::upscaler);
         return found != choices.end() ? static_cast<int>(found - choices.begin()) : 0;
     },
     [](Values& values, int choice) {
         values.graphics.upscaler = UpscalerChoices()[choice].upscaler;
     }},
    {"Upscaler Quality",
     [](const Values&) {
         return Choices({"Native AA", "Quality", "Balanced", "Performance", "Ultra Performance"});
     },
     [](const Values& values) { return static_cast<int>(values.graphics.upscale_quality); },
     [](Values& values, int choice) {
         values.graphics.upscale_quality = static_cast<Settings::UpscaleQuality>(choice);
     }},
    {"Upscaler Sharpening",
     [](const Values&) {
         Names names;
         for (const u32 level : Settings::UpscaleSharpeningLevels) {
             names.push_back(level == 0 ? std::string{"Off"} : std::to_string(level) + "%");
         }
         return names;
     },
     [](const Values& values) {
         return IndexOf(Settings::UpscaleSharpeningLevels, values.graphics.upscale_sharpening, 0);
     },
     [](Values& values, int choice) {
         values.graphics.upscale_sharpening = Settings::UpscaleSharpeningLevels[choice];
     }},
    {"Frame Generation", &SwitchNames,
     [](const Values& values) { return values.graphics.frame_generation ? 0 : 1; },
     [](Values& values, int choice) { values.graphics.frame_generation = choice == 0; }},
    {"Low Latency",
     [](const Values&) {
         Names names;
         for (const LowLatencyChoice& choice : LowLatencyChoices()) {
             names.emplace_back(choice.name);
         }
         return names;
     },
     [](const Values& values) {
         // A mode the GPU lacks runs as Universal, the last choice.
         const std::vector<LowLatencyChoice> choices = LowLatencyChoices();
         const auto found =
             std::ranges::find(choices, values.graphics.low_latency, &LowLatencyChoice::mode);
         return static_cast<int>((found != choices.end() ? found : choices.end() - 1) -
                                 choices.begin());
     },
     [](Values& values, int choice) {
         values.graphics.low_latency = LowLatencyChoices()[choice].mode;
     }},
    {"Aspect Ratio",
     [](const Values&) {
         Names names;
         for (const GamePatches::Aspect& aspect : GamePatches::Aspects) {
             names.emplace_back(aspect.name);
         }
         return names;
     },
     [](const Values& values) {
         const auto& aspects = GamePatches::Aspects;
         const auto found = std::ranges::find(aspects, std::string_view{values.game.aspect},
                                              &GamePatches::Aspect::name);
         return found != aspects.end() ? static_cast<int>(found - aspects.begin()) : 0;
     },
     [](Values& values, int choice) {
         values.game.aspect = std::string(GamePatches::Aspects[choice].name);
     }},
    {"Render Resolution",
     [](const Values&) {
         Names names;
         for (const GamePatches::RenderSize& size : GamePatches::RenderSizes) {
             names.emplace_back(size.name);
         }
         return names;
     },
     [](const Values& values) {
         const auto& sizes = GamePatches::RenderSizes;
         const auto found = std::ranges::find(sizes, std::string_view{values.game.render_size},
                                              &GamePatches::RenderSize::name);
         const auto own =
             std::ranges::find(sizes, std::string_view{"1080p"}, &GamePatches::RenderSize::name);
         return static_cast<int>((found != sizes.end() ? found : own) - sizes.begin());
     },
     [](Values& values, int choice) {
         values.game.render_size = std::string(GamePatches::RenderSizes[choice].name);
     }},
    {"Display Mode",
     [](const Values&) { return Choices({"Windowed", "Fullscreen", "Exclusive Fullscreen"}); },
     [](const Values& values) {
         return values.display.fullscreen ? (values.display.exclusive ? 2 : 1) : 0;
     },
     [](Values& values, int choice) {
         values.display.fullscreen = choice != 0;
         values.display.exclusive = choice == 2;
     }},
    {"Monitor Resolution",
     [](const Values& values) {
         Names names;
         for (const Window::Size& size : Resolutions(values)) {
             names.push_back(std::to_string(size.width) + " x " + std::to_string(size.height));
         }
         return names;
     },
     [](const Values& values) {
         const std::vector<Window::Size> sizes = Resolutions(values);
         const auto found = std::ranges::find_if(sizes, [&](const Window::Size& size) {
             return size.width == values.display.width && size.height == values.display.height;
         });
         return static_cast<int>(found - sizes.begin());
     },
     [](Values& values, int choice) {
         const Window::Size size = Resolutions(values)[choice];
         values.display.width = size.width;
         values.display.height = size.height;
     }},
    {"VSync", [](const Values&) { return Choices({"Off", "Double Buffered", "Triple Buffered"}); },
     [](const Values& values) { return static_cast<int>(values.display.vsync); },
     [](Values& values, int choice) {
         values.display.vsync = static_cast<Settings::VSync>(choice);
     }},
    {"FPS Cap",
     [](const Values&) {
         Names names;
         for (const GamePatches::FrameRate& rate : GamePatches::FrameRates) {
             names.push_back(std::to_string(rate.fps) + " FPS");
         }
         return names;
     },
     [](const Values& values) {
         const auto& rates = GamePatches::FrameRates;
         const auto found =
             std::ranges::find(rates, values.game.frame_rate, &GamePatches::FrameRate::fps);
         return found != rates.end() ? static_cast<int>(found - rates.begin()) : 0;
     },
     [](Values& values, int choice) {
         values.game.frame_rate = GamePatches::FrameRates[choice].fps;
     }},
}};

// The name of the setting's choice in values, and the count of choices to cycle through.
std::string NameOf(const Setting& setting, const Values& values) {
    const Names names = setting.names(values);
    const int choice = setting.get(values);
    return choice >= 0 && choice < static_cast<int>(names.size()) ? names[choice] : std::string{};
}

int CycleCount(const Setting& setting, const Values& values) {
    const int count = static_cast<int>(setting.names(values).size());
    return setting.custom_last ? count - 1 : count;
}

// One more System Menu row: its object, which the game reads and writes as one of its own; the
// keys its text slots got when it was made, which the game reads until the row loaded; the texts
// attached to it, its value for the selected look, its value for the plain look and its caption,
// and the keys they show; and the place it hangs from on the page.
struct OwnRow {
    alignas(32) std::array<u8, RowBytes> object{};
    std::array<std::string, 3> made_keys;
    std::array<u64, 3> texts{};
    std::array<std::string, 3> keys;
    Node place{};
    bool made = false;
    bool hung = false;
};

enum class Phase {
    // The System Menu shows its rows and the Graphics Settings row.
    List,
    // The Graphics Settings row plays its press.
    Pressing,
    // The System Menu's rows slide out.
    Leaving,
    // The page slides in.
    Opening,
    // The page takes input.
    Input,
    // The message about the next start is open.
    Notice,
    // The page slides out before the System Menu comes back.
    Backing,
    // The pause menu closes.
    Closing,
};

PlaceFunction g_place_rows = nullptr;
PageFunction g_system_frame = nullptr;
SystemModeFunction g_slide_rows = nullptr;
SystemFunction g_end_system = nullptr;

// The System object the rows belong to, and the frames its rows have loaded for.
u64 g_system = 0;
u32 g_load_frames = 0;
// A row that never loaded keeps its memory for the rest of the session, and the menu stays away.
bool g_broken = false;

OwnRow g_entry;
std::array<OwnRow, OnScreen> g_slots;

Phase g_phase = Phase::List;
bool g_close_after_notice = false;
// The settings the game started with, the ones the page shows, and those as the page opened.
Values g_started;
Values g_values;
Values g_opened;
// The setting the cursor is on, and the first setting on screen.
int g_cursor = 0;
int g_top = 0;
// The scroll bar's thumb's place down its track, from 0 to 1, and when it last moved; under 0
// while the page is not open.
float g_thumb = -1.0f;
std::chrono::steady_clock::time_point g_thumb_time;
// On a screen that is not 16:9 the game stretches its canvas, and the rows' plates keep their
// shape: the scale of a plate's width on the stretched canvas.
float g_plate_scale = 1.0f;
std::atomic<u64> g_saves{0};

std::mutex g_records_mutex;
std::map<std::string, std::vector<u8>, std::less<>> g_records;

// The scale of a plate's width on the canvas, 1 where the screen drawing keeps its 16:9 shape.
float PlateScale() {
    return Ui::FitsScreen() ? 1.0f : g_plate_scale;
}

VAddr Object(const OwnRow& row) {
    return reinterpret_cast<VAddr>(row.object.data());
}

u64 HandleOf(const OwnRow& row) {
    return Read<u64>(Object(row) + RowHandle);
}

u64 ClipOf(const OwnRow& row) {
    const u64 handle = HandleOf(row);
    return handle != 0 ? Read<u64>(handle + HandleClip) : 0;
}

bool Loaded(const OwnRow& row) {
    return row.made && Read<s32>(Object(row) + RowLoad) == RowReady;
}

bool Selected(u64 row) {
    return Read<u8>(row + RowSelected) != 0;
}

void Select(u64 row, bool selected) {
    Call<RowFlagFunction>(SelectRow)(row, selected ? 1 : 0);
}

bool Raven() {
    return std::strcmp(reinterpret_cast<const char*>(Game(Story)), "crow") == 0;
}

// The System Menu's own rows, in their order.
std::vector<u64> MenuRows(u64 system) {
    std::vector<u64> rows;
    const u64 first = Read<u64>(system + SystemListFirst);
    const u64 last = Read<u64>(system + SystemListLast);
    const size_t count = std::min<size_t>((last - first) / 8, ListRows - (Raven() ? 1 : 0));
    for (size_t i = 0; i < count; ++i) {
        if (const u64 row = Read<u64>(first + i * 8); row != 0) {
            rows.push_back(row);
        }
    }
    return rows;
}

// Makes a row as the System Menu makes its own: its clip shows keys[0], keys[1] while selected
// and the caption keys[2], at a length from 0 to 1.
bool MakeRow(OwnRow& row, const std::array<std::string, 3>& keys, float length) {
    const VAddr object = Object(row);
    row.object = {};
    row.made_keys = keys;
    row.keys = {};
    row.texts = {};
    row.hung = false;
    const u64 heap = Read<u64>(Game(Heap));
    u64 request = 0;
    if (Call<AlignFunction>(HeapAlign)(heap, &request, 0x20, RequestBytes) != 0 || request == 0) {
        return false;
    }
    Write<s32>(request, 1);
    Write<u64>(request + RequestCallback, 0);
    Write<u64>(request + RequestArgument, 0);
    Write<u64>(object, Game(RowClass));
    Write<s32>(object + RowKind, RowButton);
    Call<HandleFunction>(MakeHandle)(object + RowHandle);
    Write<float>(object + RowLength, length);
    const u64 clip = ClipOf(row);
    if (clip == 0) {
        Call<FreeFunction>(HeapFree)(heap, request);
        return false;
    }
    const auto bind = Call<BindFunction>(BindText);
    const auto width = Call<WidthFunction>(BindWidth);
    const u64 bindings = clip + ClipBindings;
    const float text_width = length * 100.0f + TextWidth;
    bind(bindings, reinterpret_cast<const char*>(Game(Text01)), row.made_keys[0].c_str());
    width(text_width, bindings, row.made_keys[0].c_str());
    bind(bindings, reinterpret_cast<const char*>(Game(Text02)), row.made_keys[1].c_str());
    width(text_width, bindings, row.made_keys[1].c_str());
    if (!row.made_keys[2].empty()) {
        bind(bindings, reinterpret_cast<const char*>(Game(Text03)), row.made_keys[2].c_str());
        width(CaptionWidth, bindings, row.made_keys[2].c_str());
        Write<u8>(object + RowCaptioned, 1);
    }
    if (Read<u32>(Game(DecideButton)) != 1) {
        bind(bindings, reinterpret_cast<const char*>(Game(CircleSlot)),
             reinterpret_cast<const char*>(Game(CrossSlot)));
    }
    const char* const asset = reinterpret_cast<const char*>(Game(RowAsset));
    Call<LoadFunction>(LoadClip)(request, clip, asset, asset);
    Call<ReserveFunction>(Reserve)(object + RowNodeList, RowNodes);
    Write<s32>(object + RowLoad, RowLoading);
    Write<u64>(request + RequestCallback, Game(RowLoaded));
    Write<u64>(request + RequestArgument, object);
    // The last reference to the request runs its callback and frees it.
    if (std::atomic_ref<s32>{*reinterpret_cast<s32*>(request)}.fetch_sub(1) == 1) {
        Call<RowFunction>(RowLoaded)(object);
        Call<FreeFunction>(HeapFree)(heap, request);
    }
    row.made = true;
    return true;
}

// Ends a row's clip. A row whose clip still loads would take the load's callback after its end,
// so it keeps its memory and the menu stays away.
void End(OwnRow& row) {
    if (!row.made) {
        return;
    }
    if (!Loaded(row)) {
        g_broken = true;
        return;
    }
    Call<RowFunction>(EndRow)(Object(row));
    row.made = false;
    row.hung = false;
    row.texts = {};
}

// The caption's node in the node list FUN_01306fd0 fills: the root, the selected look, the plain
// look, the caption, the circle and the disabled cover.
constexpr size_t CaptionNode = 3;

// Attaches the row's texts: its value twice to the node its slide moves, its caption to the
// caption's node.
void AttachTexts(OwnRow& row) {
    const u64 handle = HandleOf(row);
    const Track* const tracks = Read<const Track*>(handle + HandleFirstTrack);
    const u64 nodes = Read<u64>(Object(row) + RowNodeList + 8);
    const u32 control = tracks[ControlTrack].node;
    const std::array<AttachedText, 3> places{{
        {control, 0, 0},
        {control, 1, 0},
        {Read<u32>(nodes + CaptionNode * sizeof(u32)), 0, 0},
    }};
    for (size_t i = 0; i < row.texts.size(); ++i) {
        const u64 entry = Call<AttachFunction>(AttachText)(handle, places[i].node, places[i].index);
        row.texts[i] = entry != 0 ? Read<u64>(entry + 8) : 0;
    }
}

// After a row's frame, which draws its texts from their nodes: lifts the row's value onto its
// plate, shows the value of the look the row has, and keeps the caption over its plate's end.
void PlaceTexts(const OwnRow& row) {
    const bool selected = Selected(Object(row));
    for (size_t i = 0; i < ValueTexts; ++i) {
        const u64 text = row.texts[i];
        if (text == 0) {
            continue;
        }
        Write<float>(text + TextCanvasY, Read<float>(text + TextCanvasY) - ValueRise);
        if ((i == 0) != selected) {
            Write<u8>(text + TextShown, 0);
        }
    }
    if (const u64 caption = row.texts[ValueTexts]; caption != 0) {
        const float middle = 0.5f * CanvasWidth + row.place.world[12] * UnitPixels;
        Write<float>(caption + TextCanvasX,
                     middle + (Read<float>(caption + TextCanvasX) - middle) * PlateScale());
    }
}

void SetText(OwnRow& row, size_t index, std::string key) {
    if (row.keys[index] == key) {
        return;
    }
    if (row.texts[index] == 0) {
        return;
    }
    row.keys[index] = std::move(key);
    Call<SetKeyFunction>(SetTextKey)(row.texts[index], row.keys[index].c_str());
}

// The keys of a setting's value for the selected look and for the plain one, and of its caption.
std::array<std::string, 3> KeysOf(const Setting& setting, const Values& values) {
    const std::string name = NameOf(setting, values);
    return {std::string{SelectedValuePrefix} + name, std::string{PlainValuePrefix} + name,
            std::string{LabelPrefix} + setting.label};
}

// Shows the settings from the first one on screen in the page's rows.
void ShowSettings() {
    for (size_t i = 0; i < g_slots.size(); ++i) {
        OwnRow& slot = g_slots[i];
        if (!Loaded(slot)) {
            continue;
        }
        const int setting = g_top + static_cast<int>(i);
        std::array<std::string, 3> keys = KeysOf(Table[setting], g_values);
        for (size_t text = 0; text < keys.size(); ++text) {
            SetText(slot, text, std::move(keys[text]));
        }
    }
}

// Selects the cursor's row.
void ShowCursor() {
    for (size_t i = 0; i < g_slots.size(); ++i) {
        if (Loaded(g_slots[i])) {
            const bool selected = g_top + static_cast<int>(i) == g_cursor;
            if (Selected(Object(g_slots[i])) != selected) {
                Select(Object(g_slots[i]), selected);
            }
        }
    }
}

// Hangs the Graphics Settings row from the System Menu's empty place below its rows.
void HangEntry(u64 system) {
    const u64 layout = Read<u64>(Read<u64>(system + SystemLayout) + HandleClip);
    const u32 node = FindNode(layout, Raven() ? SixthPlace : SeventhPlace);
    const u64 clip = ClipOf(g_entry);
    if (node == NoNode || clip == 0) {
        return;
    }
    Write<u64>(clip + ClipParent, Read<u64>(layout + ClipNodes) + u64{node} * sizeof(Node));
    Call<CanvasFunction>(SetCanvas)(clip, Game(Canvas), 0, CanvasWidth, CanvasHeight);
    Call<RowFlagFunction>(ShowRow)(Object(g_entry), 1);
    g_entry.hung = true;
}

void HangSlots() {
    // FUN_013df9d0 takes its canvas transform from the place the clip hangs from, so the places
    // hold their positions before it runs.
    for (OwnRow& slot : g_slots) {
        const u64 clip = ClipOf(slot);
        if (!Loaded(slot) || clip == 0) {
            continue;
        }
        Write<u64>(clip + ClipParent, reinterpret_cast<u64>(&slot.place));
        Call<CanvasFunction>(SetCanvas)(clip, Game(Canvas), 0, CanvasWidth, CanvasHeight);
        Call<RowFlagFunction>(ShowRow)(Object(slot), 1);
        AttachTexts(slot);
        slot.hung = true;
    }
}

// Places the page's rows from the System Menu's first two places, which move with its layout.
void PlaceSlots(u64 system) {
    const u64 layout = Read<u64>(Read<u64>(system + SystemLayout) + HandleClip);
    const u32 first = FindNode(layout, FirstPlace);
    const u32 second = FindNode(layout, SecondPlace);
    if (first == NoNode || second == NoNode) {
        return;
    }
    const Node* const nodes = Read<const Node*>(layout + ClipNodes);
    const Node& top = nodes[first];
    const Node& next = nodes[second];
    for (size_t i = 0; i < g_slots.size(); ++i) {
        Node& place = g_slots[i].place;
        std::memcpy(&place, &top, sizeof(Node));
        const float steps = (FirstY + RowStep * static_cast<float>(i) - PlaceY) / PlaceStep;
        for (size_t axis = 12; axis < 15; ++axis) {
            place.world[axis] += (next.world[axis] - top.world[axis]) * steps;
        }
    }
}

void MakeRows(u64 system) {
    const std::vector<u64> rows = MenuRows(system);
    const float length = rows.empty() ? 0.0f : Read<float>(rows.front() + RowLength);
    bool made = MakeRow(g_entry, {EntryKey, EntrySelectedKey, ""}, length);
    for (size_t i = 0; i < g_slots.size(); ++i) {
        made = made && MakeRow(g_slots[i],
                               {std::string{BlankPrefix} + "1", std::string{BlankPrefix} + "2",
                                std::string{BlankPrefix} + "3"},
                               1.0f);
    }
    if (!made) {
        LOG_ERROR(Render_Vulkan, "GR2port: the Graphics Settings rows could not be made");
        g_broken = true;
    }
    PageSliders::MakeRows(system);
    g_system = system;
    g_load_frames = 0;
}

bool RowsLoaded() {
    return Loaded(g_entry) && std::ranges::all_of(g_slots, &Loaded) && PageSliders::RowsLoaded();
}

void Animate(u64 row, u32 mode) {
    Call<RowModeFunction>(AnimateRow)(row, mode);
}

void PS4_SYSV_ABI OnSlideRows(u64 system, u32 mode) {
    const bool own = system == g_system && g_entry.hung;
    const s32 category = Read<s32>(system + SystemCategory);
    // The System Menu selects its first row when it opens again; the cursor stays on the
    // Graphics Settings row.
    if (own && category == MenuCategory && mode == SlideIn && Selected(Object(g_entry))) {
        for (const u64 row : MenuRows(system)) {
            if (Selected(row)) {
                Select(row, false);
            }
        }
    }
    PageSliders::BeforeSlide(system, mode);
    Core::Recompiled::Call(g_slide_rows, system, mode);
    PageSliders::Slide(system, mode);
    if (!own || mode > SlideRest) {
        return;
    }
    if (category == MenuCategory) {
        Animate(Object(g_entry), mode);
    } else if (category == PageCategory && g_phase != Phase::List) {
        for (const OwnRow& slot : g_slots) {
            if (slot.hung) {
                Animate(Object(slot), mode);
            }
        }
    }
}

bool Slides(u64 system, u32 mode) {
    return Call<SystemTestFunction>(Sliding)(system, mode) != 0;
}

// Sets the page's header: the header of a sub-page, with the page's title.
void ShowHeader(u64 system) {
    const u64 header = Read<u64>(system + SystemHeader);
    const u32 node = FindNode(Read<u64>(header + HandleClip), HeaderText);
    if (node == NoNode) {
        return;
    }
    if (const u64 entry = Call<AttachFunction>(AttachText)(header, node, 0); entry != 0) {
        Call<SetKeyFunction>(SetTextKey)(Read<u64>(entry + 8), HeaderKey);
    }
}

void OpenGraphicsPage(u64 system) {
    g_values = Settings::Get();
    g_opened = g_values;
    ShowSettings();
    ShowCursor();
    Call<SystemModeFunction>(ShowCategory)(system, PageCategory);
    ShowHeader(system);
    OnSlideRows(system, SlideIn);
    g_phase = Phase::Opening;
}

void Move(int step) {
    const int count = static_cast<int>(Table.size());
    const int shown = static_cast<int>(g_slots.size());
    g_cursor = (g_cursor + step + count) % count;
    const int top = std::clamp(g_top, g_cursor - shown + 1, g_cursor);
    if (top != g_top) {
        g_top = top;
        ShowSettings();
    }
    ShowCursor();
    Sound(step > 0 ? DownSound : UpSound);
}

// Steps the cursor's setting to its next or previous choice. A fresh press wraps around, a held
// one stops at the end; the Custom preset steps to the first or the last preset.
void Change(int step, bool wrap) {
    const Setting& setting = Table[g_cursor];
    const int count = CycleCount(setting, g_values);
    const int choice = setting.get(g_values);
    int next = choice >= count ? (step > 0 ? 0 : count - 1) : choice + step;
    if (next < 0 || next >= count) {
        if (!wrap) {
            return;
        }
        next = (next + count) % count;
    }
    setting.set(g_values, next);
    ShowSettings();
    Sound(step > 0 ? UpSound : DownSound);
}

// The fields the page changes, from values into stored, and the preset they match.
void CopyPage(const Values& values, Values& stored) {
    const bool pass_times = stored.graphics.pass_times;
    stored.graphics = values.graphics;
    stored.graphics.pass_times = pass_times;
    stored.game.aspect = values.game.aspect;
    stored.game.render_size = values.game.render_size;
    stored.game.frame_rate = values.game.frame_rate;
    stored.display.vsync = values.display.vsync;
    stored.display.fullscreen = values.display.fullscreen;
    stored.display.exclusive = values.display.exclusive;
    stored.display.width = values.display.width;
    stored.display.height = values.display.height;
    stored.graphics.preset = std::string(Settings::MatchPreset(stored.game, stored.graphics));
}

bool Differ(const Values& a, const Values& b) {
    return std::ranges::any_of(
        Table, [&](const Setting& setting) { return setting.get(a) != setting.get(b); });
}

// Writes the page's settings to config.json on a thread of its own; a later save writes them all.
// The renderer then takes those that change while the game runs.
void Save(const Values& values) {
    const u64 number = g_saves.fetch_add(1) + 1;
    std::thread([values, number] {
        if (g_saves.load() != number) {
            return;
        }
        if (Settings::Update([&](Values& stored) { CopyPage(values, stored); })) {
            LOG_INFO(Render_Vulkan, "GR2port: the graphics settings are saved");
        }
        FieldOfView::Set(values.graphics.field_of_view);
        ScreenShake::Set(values.graphics.screen_shake);
        Window::Apply();
        Renderer::SettingsChanged();
    }).detach();
}

// Whether a setting changed in this visit to a choice the renderer does not take while the game
// runs.
bool NeedsNotice() {
    const Values live = Renderer::Live(g_started, g_values);
    return std::ranges::any_of(Table, [&](const Setting& setting) {
        const int choice = setting.get(g_values);
        return choice != setting.get(live) && choice != setting.get(g_opened);
    });
}

void Close(u64 page, u64 system) {
    Sound(BackSound);
    Call<OpenPageFunction>(OpenPage)(Read<u64>(Game(PauseMenu)), -1);
    OnSlideRows(system, SlideOut);
    Write<u8>(system + SystemKeep, 1);
    Write<s32>(page + PageState, PageClosing);
    Call<SystemFunction>(KeepValues)(system);
    Write<u8>(page + PageSaveWait, 0);
    g_phase = Phase::Closing;
}

void Back(u64 system) {
    Sound(BackSound);
    OnSlideRows(system, SlideOut);
    g_phase = Phase::Backing;
}

void Leave(u64 page, u64 system, bool close) {
    if (Differ(g_values, g_opened)) {
        Save(g_values);
    }
    if (NeedsNotice()) {
        OpenDialog(NoticeKey, 0, -1);
        g_close_after_notice = close;
        g_phase = Phase::Notice;
        return;
    }
    if (close) {
        Close(page, system);
    } else {
        Back(system);
    }
}

void TakeInput(u64 page, u64 system, u64 input) {
    if (Read<u8>(input + InputEnabled) == 0) {
        return;
    }
    const u32 pressed = Read<u32>(input + InputPressed);
    const u32 held = Read<u32>(input + InputHeld);
    const u32 repeated = Read<u32>(input + InputRepeated);
    // With both directions of an axis held, the axis moves nothing.
    const bool vertical = (held & UpButtons) == 0 || (held & DownButtons) == 0;
    const bool horizontal = (held & LeftButtons) == 0 || (held & RightButtons) == 0;
    if ((pressed & OptionsButton) != 0) {
        Leave(page, system, true);
    } else if ((pressed & ButtonBit(CancelButton)) != 0) {
        Leave(page, system, false);
    } else if ((pressed & ButtonBit(DecideButton)) != 0) {
        Change(1, true);
    } else if (vertical && (repeated & DownButtons) != 0) {
        Move(1);
    } else if (vertical && (repeated & UpButtons) != 0) {
        Move(-1);
    } else if (horizontal && (repeated & RightButtons) != 0) {
        Change(1, (pressed & RightButtons) != 0);
    } else if (horizontal && (repeated & LeftButtons) != 0) {
        Change(-1, (pressed & LeftButtons) != 0);
    }
}

// Moves the System Menu's selection onto the Graphics Settings row and off it ahead of the page's
// own input code, and starts the press when the row is chosen. True when the page must not see
// this frame's input.
bool Steer(u64 page, u64 system, u64 input) {
    if (Read<s32>(page + PageState) != PageInput || Read<u8>(input + InputEnabled) == 0) {
        return false;
    }
    const u32 pressed = Read<u32>(input + InputPressed);
    if ((pressed & (OptionsButton | ButtonBit(CancelButton))) != 0) {
        return false;
    }
    const std::vector<u64> rows = MenuRows(system);
    if (rows.empty()) {
        return false;
    }
    const u32 held = Read<u32>(input + InputHeld);
    const u32 repeated = Read<u32>(input + InputRepeated);
    const bool vertical = (held & UpButtons) == 0 || (held & DownButtons) == 0;
    const bool up = vertical && (repeated & UpButtons) != 0;
    const bool down = vertical && (repeated & DownButtons) != 0;
    const bool decide = (pressed & ButtonBit(DecideButton)) != 0;
    const u64 entry = Object(g_entry);
    const auto move = [](u64 from, u64 to, u32 sound) {
        Select(from, false);
        Select(to, true);
        Sound(sound);
        return true;
    };
    if (Selected(entry)) {
        if (decide) {
            Animate(entry, Press);
            Sound(DecideSound);
            g_phase = Phase::Pressing;
            return true;
        }
        if (down) {
            return move(entry, rows.front(), DownSound);
        }
        if (up) {
            return move(entry, rows.back(), UpSound);
        }
        return false;
    }
    if (decide) {
        return false;
    }
    if (down && Selected(rows.back())) {
        return move(rows.back(), entry, DownSound);
    }
    if (up && Selected(rows.front())) {
        return move(rows.front(), entry, UpSound);
    }
    return false;
}

// The page's frame ahead of the System Menu page's own. True when that page must not see this
// frame's input.
bool Run(u64 page, u64 system, u64 input) {
    switch (g_phase) {
    case Phase::List:
        return Steer(page, system, input);
    case Phase::Pressing: {
        const u64 handle = HandleOf(g_entry);
        if (!Playing(handle, ControlTrack, PressAnimation) &&
            !Playing(handle, PressSecondTrack, 0)) {
            OnSlideRows(system, SlideOut);
            g_phase = Phase::Leaving;
        }
        return true;
    }
    case Phase::Leaving:
        if (!Slides(system, SlideOut)) {
            OpenGraphicsPage(system);
        }
        return true;
    case Phase::Opening:
        if (!Slides(system, SlideIn)) {
            g_phase = Phase::Input;
        }
        return true;
    case Phase::Input:
        TakeInput(page, system, input);
        return true;
    case Phase::Notice:
        if (Answer() >= 0) {
            if (g_close_after_notice) {
                Close(page, system);
            } else {
                Back(system);
            }
        }
        return true;
    case Phase::Backing:
        if (!Slides(system, SlideOut)) {
            Write<s32>(page + PageState, PageEnter);
            g_phase = Phase::List;
        }
        return true;
    case Phase::Closing:
        return true;
    }
    return false;
}

// A row's opacity: its place's, which the System Menu's slides fade, times its own slide's.
float Opacity(const OwnRow& row) {
    const u64 handle = HandleOf(row);
    const u64 clip = handle != 0 ? Read<u64>(handle + HandleClip) : 0;
    if (clip == 0) {
        return 0.0f;
    }
    const Track* const tracks = Read<const Track*>(handle + HandleFirstTrack);
    const Node* const nodes = Read<const Node*>(clip + ClipNodes);
    if (tracks == nullptr || nodes == nullptr) {
        return 0.0f;
    }
    return std::clamp(row.place.opacity, 0.0f, 1.0f) *
           std::clamp(nodes[tracks[ControlTrack].node].opacity, 0.0f, 1.0f);
}

// Shows where the page's rows lie among all the settings, on a bar beside them.
void ShowBar() {
    const float count = static_cast<float>(Table.size());
    const float shown = static_cast<float>(g_slots.size());
    if (g_phase == Phase::List || g_phase == Phase::Pressing || g_phase == Phase::Leaving ||
        count <= shown || !Loaded(g_slots.front())) {
        g_thumb = -1.0f;
        UiCapture::SetScrollBar({});
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float wanted = static_cast<float>(g_top) / (count - shown);
    if (g_thumb < 0.0f) {
        g_thumb = wanted;
    } else {
        const float seconds = std::chrono::duration<float>(now - g_thumb_time).count();
        g_thumb += (wanted - g_thumb) * (1.0f - std::exp(-seconds / ThumbTime));
    }
    g_thumb_time = now;
    const Node& first = g_slots.front().place;
    const Node& last = g_slots.back().place;
    const float top = 0.5f * CanvasHeight - first.world[13] * UnitPixels - BarAbove;
    const float bottom = 0.5f * CanvasHeight - last.world[13] * UnitPixels + BarBelow;
    const float thumb = std::max(MinThumb, (bottom - top) * shown / count);
    const float thumb_top = top + (bottom - top - thumb) * g_thumb;
    UiCapture::SetScrollBar({
        .x = 0.5f * CanvasWidth + first.world[12] * UnitPixels + BarRight * PlateScale(),
        .width = BarWidth * PlateScale(),
        .top = top,
        .bottom = bottom,
        .thumb_top = thumb_top,
        .thumb_bottom = thumb_top + thumb,
        .opacity = Opacity(g_slots.front()) * (g_phase == Phase::Notice ? NoticeOpacity : 1.0f),
    });
}

void Tick(u64 system) {
    if (g_phase != Phase::List) {
        PlaceSlots(system);
    }
    const auto tick = Call<HandleFunction>(TickClip);
    if (Loaded(g_entry)) {
        tick(HandleOf(g_entry));
    }
    for (const OwnRow& slot : g_slots) {
        if (Loaded(slot)) {
            tick(HandleOf(slot));
            PlaceTexts(slot);
        }
    }
    ShowBar();
}

u64 PS4_SYSV_ABI OnPlaceRows(u64 system) {
    if (!g_broken && Read<s32>(system + SystemLoad) == SystemClipsLoaded) {
        if (g_system != system) {
            MakeRows(system);
        }
        if (!RowsLoaded()) {
            if (++g_load_frames < LoadFrames) {
                return 0;
            }
            LOG_ERROR(Render_Vulkan, "GR2port: the Graphics Settings rows did not load");
            g_broken = true;
        }
    }
    const u64 placed = Core::Recompiled::Call(g_place_rows, system);
    if (placed != 0 && !g_broken && system == g_system && !g_entry.hung && RowsLoaded()) {
        HangEntry(system);
        PlaceSlots(system);
        HangSlots();
        PageSliders::Hang(system);
    }
    return placed;
}

void PS4_SYSV_ABI OnSystemFrame(u64 page) {
    const u64 system = Read<u64>(Game(SystemObject));
    if (g_broken || system == 0 || system != g_system || !g_entry.hung ||
        Read<u8>(page + PageActive) == 0) {
        Core::Recompiled::Call(g_system_frame, page);
        return;
    }
    const u64 input = Read<u64>(Read<u64>(Game(PauseMenu)) + PauseMenuInput);
    if (Run(page, system, input)) {
        const u8 enabled = Read<u8>(input + InputEnabled);
        Write<u8>(input + InputEnabled, 0);
        Core::Recompiled::Call(g_system_frame, page);
        Write<u8>(input + InputEnabled, enabled);
    } else {
        Core::Recompiled::Call(g_system_frame, page);
    }
    Tick(system);
}

void PS4_SYSV_ABI OnEndSystem(u64 system) {
    if (system == g_system) {
        End(g_entry);
        for (OwnRow& slot : g_slots) {
            End(slot);
        }
        PageSliders::End(system);
        g_system = 0;
        g_phase = Phase::List;
        g_thumb = -1.0f;
        UiCapture::SetScrollBar({});
    }
    Core::Recompiled::Call(g_end_system, system);
}

template <typename Function>
bool Replace(VAddr module_base, const Hooks::Entry& entry, Xbyak::CodeGenerator& c, size_t capacity,
             Function target, Function& original) {
    const void* trampoline = nullptr;
    if (Hooks::Replace(module_base, entry, c, capacity, reinterpret_cast<const void*>(target),
                       &trampoline) != Hooks::Result::Hooked) {
        return false;
    }
    original = reinterpret_cast<Function>(const_cast<void*>(trampoline));
    return true;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    SetBase(module_base);
    g_started = Settings::Get();
    g_plate_scale = static_cast<float>(CanvasWidth) / CanvasHeight / GamePatches::AspectRatio();
    // With room in the stub area for each and every entry as expected, the hooks go in all
    // together or not at all.
    const std::array entries{PlaceRows, SystemFrame, SlideRows, EndSystem};
    if (capacity - c.getSize() < entries.size() * Hooks::MaxStubBytes ||
        !std::ranges::all_of(
            entries, [&](const Hooks::Entry& entry) { return Hooks::Holds(module_base, entry); }) ||
        !Replace(module_base, PlaceRows, c, capacity, &OnPlaceRows, g_place_rows) ||
        !Replace(module_base, SystemFrame, c, capacity, &OnSystemFrame, g_system_frame) ||
        !Replace(module_base, SlideRows, c, capacity, &OnSlideRows, g_slide_rows) ||
        !Replace(module_base, EndSystem, c, capacity, &OnEndSystem, g_end_system)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the System Menu is not the expected code");
        return false;
    }
    PageSliders::Install(module_base, c, capacity);
    LOG_INFO(Render_Vulkan, "GR2port: the System Menu has a Graphics Settings row");
    return true;
}

u64 FindText(u64 texts, const char* key, FindTextFunction find) {
    const std::string_view name{key};
    std::scoped_lock lock{g_records_mutex};
    if (const auto found = g_records.find(name); found != g_records.end()) {
        return reinterpret_cast<u64>(found->second.data());
    }
    const char* model = nullptr;
    std::string_view english;
    if (name == EntryKey) {
        model = "ui_system_menu_1_1_6";
        english = Title;
    } else if (name == EntrySelectedKey) {
        model = "ui_system_menu_2_1_6";
        english = Title;
    } else if (name == HeaderKey) {
        model = "ui_system_menu_header_2_6";
        english = Title;
    } else if (name == NoticeKey) {
        model = "ui_menu_yesno_system_1";
        english = Notice;
    } else if (name.starts_with(BlankPrefix)) {
        const std::string_view slot = name.substr(BlankPrefix.size());
        model = slot == "1"   ? "ui_system_menu_1_6_4"
                : slot == "2" ? "ui_system_menu_2_6_4"
                              : "ui_system_menu_3_4_1";
    } else if (name.starts_with(LabelPrefix)) {
        model = "ui_system_menu_3_4_1";
        english = name.substr(LabelPrefix.size());
    } else if (name.starts_with(SelectedValuePrefix)) {
        model = "ui_system_menu_1_6_4";
        english = name.substr(SelectedValuePrefix.size());
    } else if (name.starts_with(PlainValuePrefix)) {
        model = "ui_system_menu_2_6_4";
        english = name.substr(PlainValuePrefix.size());
    } else if (name.starts_with(PageSliders::KeyPrefix)) {
        model = PageSliders::ModelKey;
        english = name.substr(PageSliders::KeyPrefix.size());
    } else {
        return 0;
    }
    const u64 record = Core::Recompiled::Call(find, texts, model);
    if (record == 0) {
        return 0;
    }
    const auto made = g_records.emplace(std::string{name}, MakeRecord(record, Shown(english)));
    return reinterpret_cast<u64>(made.first->second.data());
}

void AfterDrawClip(u64 handle, u64 context, DrawFunction draw) {
    const u64 system = g_system;
    if (system == 0 || g_broken || Read<u64>(Game(SystemObject)) != system) {
        return;
    }
    const u64 rows = Read<u64>(system + SystemRows);
    const u64 first = rows != 0 ? Read<u64>(rows) : 0;
    if (first == 0 || handle != Read<u64>(first + RowHandle)) {
        return;
    }
    const s32 category = Read<s32>(system + SystemCategory);
    if (category == MenuCategory && g_entry.hung) {
        Core::Recompiled::Call(draw, HandleOf(g_entry), context);
    } else if (category == PageCategory && g_phase != Phase::List) {
        for (const OwnRow& slot : g_slots) {
            if (slot.hung) {
                Core::Recompiled::Call(draw, HandleOf(slot), context);
            }
        }
    }
    PageSliders::Draw(system, context, draw);
}

void SetGpuVendor(u32 vendor) {
    g_vendor.store(vendor, std::memory_order_relaxed);
}

} // namespace Gr2Port::GraphicsMenu
