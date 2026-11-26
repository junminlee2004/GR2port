// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/camera_menu.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/menu_kit.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/special_cameras.h"
#include "graphics/gr2port/ui_capture.h"

namespace Gr2Port::CameraMenu {

namespace {

using namespace MenuKit;

// void FUN_01307eb0(page): the frame of the Camera Configuration page.
constexpr Hooks::Entry CameraPage{0x1307eb0, 6, Hooks::FramePrologue};

// void FUN_01394df0(u64* handle): makes a clip handle and its clip.
constexpr VAddr MakeHandle = 0x1394df0;
// void FUN_013db4a0(bindings, slot, key) and FUN_013db1a0(float width, bindings, key): name the
// key a clip's text slot shows and the text's width, before the clip loads.
constexpr VAddr BindText = 0x13db4a0;
constexpr VAddr BindWidth = 0x13db1a0;
// void FUN_013e10f0(request, clip, asset, name): loads a clip. The request's callback runs when
// its last reference is dropped.
constexpr VAddr LoadClip = 0x13e10f0;
// void FUN_01313390(slider): finishes a slider once its clip loaded.
constexpr VAddr SliderLoaded = 0x1313390;
// void FUN_01313650(slider): ends a slider's clip handle and frees its node list; the slider
// stays.
constexpr VAddr EndSlider = 0x1313650;
// void FUN_013df9d0(clip, canvas, 0, width, height): sets the canvas a clip draws on.
constexpr VAddr SetCanvas = 0x13df9d0;
// The interface's heap, and its posix_memalign and free.
constexpr VAddr Heap = 0x1cdf970;
constexpr VAddr HeapAlign = 0x17f56f0;
constexpr VAddr HeapFree = 0x17f56b0;
// The canvas the interface's clips draw on, 1920 x 1080.
constexpr VAddr Canvas = 0x1eeeb70;
constexpr u32 CanvasWidth = 1920;
constexpr u32 CanvasHeight = 1080;
// The object every System page shares.
constexpr VAddr SystemObject = 0x1eee210;
// The game's names: the slider clip and its caption's text slot.
constexpr VAddr SliderAsset = 0x18f4970;
constexpr VAddr Text01 = 0x18f7194;
// The class of a System Menu slider.
constexpr VAddr SliderClass = 0x1ae8370;

// The System object: the category it shows, and the rows of the camera page, category 3: the two
// pairs of buttons of the vertical and horizontal controls, Reset to Defaults, then the sliders
// Motion Sensitivity and Camera Sensitivity.
constexpr u64 SystemCategory = 0x10c;
constexpr s32 CameraCategory = 3;
constexpr u64 CameraRows = 0x90;
constexpr size_t PageRows = 7;
constexpr size_t SecondPairLeft = 2;
constexpr size_t SecondPairRight = 3;
constexpr size_t ResetRow = 4;
constexpr size_t FirstSlider = 5;

// The camera page's state: it enters, slides in, takes input, plays the press of Reset to
// Defaults, slides out.
constexpr u64 PageState = 0x08;
constexpr s32 PageEnter = 0;
constexpr s32 PageInput = 2;
constexpr s32 PageReset = 3;
constexpr s32 PageClosing = 4;

// A menu's input: the frames between the repeats of a held button. A slider's button held for
// FastRepeats frames steps by FastStep and repeats every FastFrames frames.
constexpr u64 InputRepeatFrames = 0x84;
constexpr u32 FastFrames = 2;
constexpr u32 FastRepeats = 10;
constexpr s32 FastStep = 2;

// A System Menu row: its class, its kind and its clip handle. The class has the row's frame, its
// show, its select and its selected test in these slots.
constexpr u64 RowKind = 0x08;
constexpr u64 RowHandle = 0x10;
constexpr u64 TickSlot = 0x10;
constexpr u64 ShowSlot = 0x28;
constexpr u64 SelectSlot = 0x30;
constexpr u64 SelectedSlot = 0x38;
constexpr s32 SliderKind = 2;
// A slider: its load (1 while its clip loads, 2 once loaded), its value from 0 to 100 with 50 for
// the game's own, its lowest and its highest value.
constexpr size_t SliderBytes = 0x50;
constexpr u64 SliderLoad = 0x38;
constexpr u64 SliderValue = 0x3c;
constexpr u64 SliderLow = 0x44;
constexpr u64 SliderHigh = 0x48;
constexpr s32 SliderLoading = 1;
constexpr s32 SliderReady = 2;
constexpr s32 SliderTop = 100;
constexpr s32 SliderDefault = 50;
constexpr float CaptionWidth = 580.0f;
// A clip's text bindings.
constexpr u64 ClipBindings = 0x08;
// A load request: its references, its callback and the callback's argument.
constexpr u64 RequestBytes = 0x18;
constexpr u64 RequestCallback = 0x08;
constexpr u64 RequestArgument = 0x10;
// A slider's slides, as FUN_01310660 plays them. Track 0 moves the slider, and its animation 3
// gives the selected look; track 1 moves the knob.
constexpr u32 SlideIn = 0;
constexpr u32 SlideOut = 1;
constexpr size_t SlideTrack = 0;
constexpr size_t KnobTrack = 1;
constexpr size_t InAnimation = 0;
constexpr size_t OutAnimation = 1;
constexpr size_t SelectedAnimation = 3;

constexpr u32 DownSound = 0x33f3f50e;
constexpr u32 UpSound = 0xda16c583;

// A camera sensitivity: its caption and the setting that holds it, in percent of the game's.
struct Sensitivity {
    const char* caption;
    u32 Settings::Mouse::* percent;
};
constexpr std::array<Sensitivity, 2> Sensitivities{{
    {"Special Attack Camera Sensitivity", &Settings::Mouse::special_attack_camera},
    {"Panther Mode Camera Sensitivity", &Settings::Mouse::panther_camera},
}};
using Percents = std::array<u32, Sensitivities.size()>;

// The page lists its own two sliders, then the new ones, and shows two of them.
constexpr int PageSliders = 2;
constexpr int Listed = PageSliders + static_cast<int>(Sensitivities.size());
constexpr int OnScreen = 2;
using List = std::array<u64, Listed>;

// A layout unit is 100 canvas pixels, from the canvas's middle, with y upwards. The scroll bar
// stands beside the sliders on screen: its middle BarRight canvas pixels right of theirs, from
// BarAbove above the first place, over its caption, to BarBelow below the second, under its knob.
// Its thumb is as long as the share of the listed sliders on screen, and follows the list in
// ThumbTime.
constexpr float UnitPixels = 100.0f;
constexpr float BarRight = 330.0f;
constexpr float BarWidth = 10.0f;
constexpr float BarAbove = 72.0f;
constexpr float BarBelow = 24.0f;
constexpr float ThumbTime = 0.06f;

// One more slider: its object, which the game reads and writes as one of its own, and the key of
// its caption, which the game reads until the slider loaded.
struct Slider {
    alignas(32) std::array<u8, SliderBytes> object{};
    std::string key;
    bool made = false;
};

using HandleFunction = void PS4_SYSV_ABI (*)(u64 handle);
using BindFunction = void PS4_SYSV_ABI (*)(u64 bindings, const char* slot, const char* key);
using WidthFunction = void PS4_SYSV_ABI (*)(float width, u64 bindings, const char* key);
using LoadFunction = void PS4_SYSV_ABI (*)(u64 request, u64 clip, const char* asset,
                                           const char* name);
using RowFunction = void PS4_SYSV_ABI (*)(u64 row);
using RowFlagFunction = void PS4_SYSV_ABI (*)(u64 row, u8 flag);
using RowTestFunction = u8 PS4_SYSV_ABI (*)(u64 row);
using CanvasFunction = void PS4_SYSV_ABI (*)(u64 clip, u64 canvas, u32 zero, u32 width, u32 height);
using PageFunction = void PS4_SYSV_ABI (*)(u64 page);
using AlignFunction = s32 PS4_SYSV_ABI (*)(u64 heap, u64* block, u64 alignment, u64 size);
using FreeFunction = void PS4_SYSV_ABI (*)(u64 heap, u64 block);

PageFunction g_camera_page = nullptr;
std::array<Slider, Sensitivities.size()> g_sliders;
// The System object the sliders belong to; once they hang on its page, the page's rows, the
// listed sliders and the places of the page's two sliders.
u64 g_system = 0;
bool g_hung = false;
std::array<u64, PageRows> g_rows{};
List g_list{};
std::array<u64, OnScreen> g_places{};
// The first listed slider on screen.
int g_top = 0;
// Where the scroll bar's thumb is, from 0 at the top to 1 at the bottom, or -1 until the page
// shows it, and when it last moved.
float g_thumb = -1.0f;
std::chrono::steady_clock::time_point g_thumb_time;
// The frames Right and Left were held, up to FastRepeats.
u32 g_right = 0;
u32 g_left = 0;
// The sensitivities as the page opened.
Percents g_opened{};
// A slider that never loaded keeps its memory until the game exits, and the page stays as the
// game made it.
bool g_broken = false;
std::atomic<u64> g_saves{0};

VAddr Object(const Slider& slider) {
    return reinterpret_cast<VAddr>(slider.object.data());
}

bool Loaded(const Slider& slider) {
    return slider.made && Read<s32>(Object(slider) + SliderLoad) == SliderReady;
}

u64 ClipOf(u64 row) {
    const u64 handle = Read<u64>(row + RowHandle);
    return handle != 0 ? Read<u64>(handle + HandleClip) : 0;
}

template <typename Function>
Function Method(u64 row, u64 slot) {
    return reinterpret_cast<Function>(Read<u64>(Read<u64>(row) + slot));
}

void Show(u64 row, bool shown) {
    Method<RowFlagFunction>(row, ShowSlot)(row, shown ? 1 : 0);
}

void Select(u64 row, bool selected) {
    Method<RowFlagFunction>(row, SelectSlot)(row, selected ? 1 : 0);
}

bool Selected(u64 row) {
    return Method<RowTestFunction>(row, SelectedSlot)(row) != 0;
}

Percents PercentsOf() {
    Percents percents{};
    for (size_t i = 0; i < g_sliders.size(); ++i) {
        percents[i] = static_cast<u32>(Read<s32>(Object(g_sliders[i]) + SliderValue)) * 2;
    }
    return percents;
}

void Apply() {
    const Percents percents = PercentsOf();
    SpecialCameras::Set(percents[0], percents[1]);
}

// Writes the sensitivities to config.json on a thread of its own; a later save writes them all.
void Save(const Percents& percents) {
    const u64 number = g_saves.fetch_add(1) + 1;
    std::thread([percents, number] {
        if (g_saves.load() != number) {
            return;
        }
        if (Settings::Update([&](Settings::Values& stored) {
                for (size_t i = 0; i < Sensitivities.size(); ++i) {
                    stored.mouse.*Sensitivities[i].percent = percents[i];
                }
            })) {
            LOG_INFO(Render_Vulkan, "GR2port: the camera sensitivities are saved");
        }
    }).detach();
}

// Makes a slider as the System Menu makes its own, at value.
bool Make(Slider& slider, const Sensitivity& sensitivity, s32 value) {
    const VAddr object = Object(slider);
    slider.object = {};
    slider.key = std::string{KeyPrefix} + sensitivity.caption;
    const u64 heap = Read<u64>(Game(Heap));
    u64 request = 0;
    if (Call<AlignFunction>(HeapAlign)(heap, &request, 0x20, RequestBytes) != 0 || request == 0) {
        return false;
    }
    Write<s32>(request, 1);
    Write<u64>(request + RequestCallback, 0);
    Write<u64>(request + RequestArgument, 0);
    Write<u64>(object, Game(SliderClass));
    Write<s32>(object + RowKind, SliderKind);
    Write<s32>(object + SliderHigh, SliderTop);
    Write<s32>(object + SliderValue, value);
    Call<HandleFunction>(MakeHandle)(object + RowHandle);
    const u64 clip = ClipOf(object);
    if (clip == 0) {
        Call<FreeFunction>(HeapFree)(heap, request);
        return false;
    }
    const u64 bindings = clip + ClipBindings;
    Call<BindFunction>(BindText)(bindings, reinterpret_cast<const char*>(Game(Text01)),
                                 slider.key.c_str());
    Call<WidthFunction>(BindWidth)(CaptionWidth, bindings, slider.key.c_str());
    const char* const asset = reinterpret_cast<const char*>(Game(SliderAsset));
    Call<LoadFunction>(LoadClip)(request, clip, asset, asset);
    Write<s32>(object + SliderLoad, SliderLoading);
    Write<u64>(request + RequestCallback, Game(SliderLoaded));
    Write<u64>(request + RequestArgument, object);
    // The last reference to the request runs its callback and frees it.
    if (std::atomic_ref<s32>{*reinterpret_cast<s32*>(request)}.fetch_sub(1) == 1) {
        Call<RowFunction>(SliderLoaded)(object);
        Call<FreeFunction>(HeapFree)(heap, request);
    }
    slider.made = true;
    return true;
}

void HangFrom(u64 row, u64 place) {
    const u64 clip = ClipOf(row);
    if (Read<u64>(clip + ClipParent) == place) {
        return;
    }
    Write<u64>(clip + ClipParent, place);
    Call<CanvasFunction>(SetCanvas)(clip, Game(Canvas), 0, CanvasWidth, CanvasHeight);
}

// Hangs the listed sliders from the first one on screen in the page's two places, and hides the
// others.
void ShowList() {
    for (int i = 0; i < Listed; ++i) {
        const bool on_screen = i >= g_top && i < g_top + OnScreen;
        if (on_screen) {
            HangFrom(g_list[i], g_places[i - g_top]);
        }
        Show(g_list[i], on_screen);
    }
}

// The listed slider the cursor is on, or Listed.
int Cursor() {
    for (int i = g_top; i < g_top + OnScreen; ++i) {
        if (Selected(g_list[i])) {
            return i;
        }
    }
    return Listed;
}

void Move(int from, int to) {
    Select(g_list[from], false);
    const int top = std::clamp(g_top, to - OnScreen + 1, to);
    if (top != g_top) {
        g_top = top;
        ShowList();
    }
    Select(g_list[to], true);
    Sound(to > from ? DownSound : UpSound);
}

// Steps a new slider's value as the page steps its own: by 1, by FastStep once its button was held
// for FastRepeats frames, and around to the other end on a fresh press at an end.
void Step(u64 slider, s32 sign, u32 held) {
    const s32 value = Read<s32>(slider + SliderValue);
    const s32 low = Read<s32>(slider + SliderLow);
    const s32 high = Read<s32>(slider + SliderHigh);
    const s32 next =
        held < 2 && value == (sign > 0 ? high : low)
            ? (sign > 0 ? low : high)
            : std::clamp(value + sign * (held >= FastRepeats ? FastStep : 1), low, high);
    if (next == value) {
        return;
    }
    Write<s32>(slider + SliderValue, next);
    Apply();
    Sound(sign > 0 ? UpSound : DownSound);
}

// Moves the cursor through the listed sliders and steps the new ones ahead of the page's own input
// code, in the page's order of Down, Left, Up and Right. True when the page must not see this
// frame's input.
bool Steer(u64 input) {
    if (Read<u8>(input + InputEnabled) == 0) {
        return false;
    }
    const u32 pressed = Read<u32>(input + InputPressed);
    const u32 held = Read<u32>(input + InputHeld);
    const u32 repeated = Read<u32>(input + InputRepeated);
    const bool horizontal = (held & LeftButtons) == 0 || (held & RightButtons) == 0;
    if (horizontal) {
        g_right = (held & RightButtons) != 0 ? std::min(g_right + 1, FastRepeats) : 0;
        g_left = (held & LeftButtons) != 0 ? std::min(g_left + 1, FastRepeats) : 0;
    }
    const u32 page_buttons = OptionsButton | ButtonBit(CancelButton) | ButtonBit(DecideButton);
    if ((pressed & page_buttons) != 0) {
        return false;
    }
    const bool vertical = (held & UpButtons) == 0 || (held & DownButtons) == 0;
    const bool down = vertical && (repeated & DownButtons) != 0;
    const bool up = vertical && (repeated & UpButtons) != 0;
    const bool left = horizontal && (repeated & LeftButtons) != 0;
    const bool right = horizontal && (repeated & RightButtons) != 0;
    const int at = Cursor();
    if (at == Listed) {
        // The page moves down from its second pair of buttons onto its first slider.
        if (down && g_top != 0 &&
            (Selected(g_rows[SecondPairLeft]) || Selected(g_rows[SecondPairRight]))) {
            g_top = 0;
            ShowList();
        }
        if (!down && up && Selected(g_rows[ResetRow])) {
            Select(g_rows[ResetRow], false);
            g_top = Listed - OnScreen;
            ShowList();
            Select(g_list[Listed - 1], true);
            Sound(UpSound);
            return true;
        }
        return false;
    }
    if (down) {
        if (at + 1 < Listed) {
            Move(at, at + 1);
        } else {
            Select(g_list[at], false);
            Select(g_rows[ResetRow], true);
            Sound(DownSound);
        }
        return true;
    }
    if (at < PageSliders) {
        // The page steps its own sliders and moves up off the first.
        if (!left && up && at > 0) {
            Move(at, at - 1);
            return true;
        }
        return false;
    }
    if (left) {
        Step(g_list[at], -1, g_left);
    } else if (up) {
        Move(at, at - 1);
    } else if (right) {
        Step(g_list[at], 1, g_right);
    }
    return true;
}

void Open() {
    for (const Slider& slider : g_sliders) {
        if (Selected(Object(slider))) {
            Select(Object(slider), false);
        }
    }
    g_top = 0;
    ShowList();
    g_thumb = -1.0f;
    g_right = 0;
    g_left = 0;
    g_opened = PercentsOf();
}

// Shows where the sliders on screen lie among the listed ones, on a bar beside them that comes and
// goes with them. The System Menu's list hangs from the same places, and fades them in again as the
// page goes.
void ShowBar() {
    const Node& first = *reinterpret_cast<const Node*>(g_places.front());
    const Node& last = *reinterpret_cast<const Node*>(g_places.back());
    const auto now = std::chrono::steady_clock::now();
    const float wanted = static_cast<float>(g_top) / static_cast<float>(Listed - OnScreen);
    if (g_thumb < 0.0f) {
        g_thumb = wanted;
    } else {
        const float seconds = std::chrono::duration<float>(now - g_thumb_time).count();
        g_thumb += (wanted - g_thumb) * (1.0f - std::exp(-seconds / ThumbTime));
    }
    g_thumb_time = now;
    const float top = 0.5f * CanvasHeight - first.world[13] * UnitPixels - BarAbove;
    const float bottom = 0.5f * CanvasHeight - last.world[13] * UnitPixels + BarBelow;
    const float thumb = (bottom - top) * OnScreen / Listed;
    const float thumb_top = top + (bottom - top - thumb) * g_thumb;
    UiCapture::SetScrollBar({
        .x = 0.5f * CanvasWidth + first.world[12] * UnitPixels + BarRight,
        .width = BarWidth,
        .top = top,
        .bottom = bottom,
        .thumb_top = thumb_top,
        .thumb_bottom = thumb_top + thumb,
        .opacity = Read<s32>(g_system + SystemCategory) == CameraCategory
                       ? std::clamp(first.opacity, 0.0f, 1.0f)
                       : 0.0f,
    });
}

void PS4_SYSV_ABI OnCameraPage(u64 page) {
    if (!g_hung || Read<u64>(Game(SystemObject)) != g_system) {
        Core::Recompiled::Call(g_camera_page, page);
        return;
    }
    const u64 input = Read<u64>(Read<u64>(Game(PauseMenu)) + PauseMenuInput);
    const s32 state = Read<s32>(page + PageState);
    if (state == PageEnter) {
        Open();
    }
    if (state == PageInput && Steer(input)) {
        const u8 enabled = Read<u8>(input + InputEnabled);
        Write<u8>(input + InputEnabled, 0);
        Core::Recompiled::Call(g_camera_page, page);
        Write<u8>(input + InputEnabled, enabled);
        // The page's input code sets the repeats' pace from its own counts.
        if (std::max(g_right, g_left) >= FastRepeats) {
            Write<u32>(input + InputRepeatFrames, FastFrames);
        }
    } else {
        Core::Recompiled::Call(g_camera_page, page);
    }
    const s32 next = Read<s32>(page + PageState);
    if (state == PageInput && next == PageReset) {
        for (const Slider& slider : g_sliders) {
            Write<s32>(Object(slider) + SliderValue, SliderDefault);
        }
        Apply();
    } else if (state == PageInput && next == PageClosing) {
        if (const Percents percents = PercentsOf(); percents != g_opened) {
            Save(percents);
        }
    }
    for (const Slider& slider : g_sliders) {
        Method<RowFunction>(Object(slider), TickSlot)(Object(slider));
    }
    ShowBar();
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    const void* trampoline = nullptr;
    if (capacity - c.getSize() < Hooks::MaxStubBytes || !Hooks::Holds(module_base, CameraPage) ||
        Hooks::Replace(module_base, CameraPage, c, capacity,
                       reinterpret_cast<const void*>(&OnCameraPage),
                       &trampoline) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the Camera Configuration page is not the expected code");
        return false;
    }
    g_camera_page = reinterpret_cast<PageFunction>(const_cast<void*>(trampoline));
    return true;
}

void MakeRows(u64 system) {
    if (g_camera_page == nullptr || g_broken) {
        return;
    }
    const Settings::Values values = Settings::Get();
    for (size_t i = 0; i < g_sliders.size(); ++i) {
        const u32 percent = values.mouse.*Sensitivities[i].percent;
        if (!Make(g_sliders[i], Sensitivities[i], static_cast<s32>(percent / 2))) {
            LOG_ERROR(Render_Vulkan, "GR2port: the camera sensitivity sliders could not be made");
            g_broken = true;
            return;
        }
    }
    g_system = system;
}

bool RowsLoaded() {
    return g_system == 0 || g_broken || std::ranges::all_of(g_sliders, &Loaded);
}

void Hang(u64 system) {
    if (system != g_system || g_broken || g_hung) {
        return;
    }
    const u64 rows = Read<u64>(system + CameraRows);
    for (size_t i = 0; i < PageRows; ++i) {
        g_rows[i] = Read<u64>(rows + i * sizeof(u64));
    }
    g_list = {g_rows[FirstSlider], g_rows[FirstSlider + 1], Object(g_sliders[0]),
              Object(g_sliders[1])};
    for (int i = 0; i < OnScreen; ++i) {
        g_places[i] = Read<u64>(ClipOf(g_rows[FirstSlider + i]) + ClipParent);
    }
    for (const Slider& slider : g_sliders) {
        HangFrom(Object(slider), g_places.back());
        Show(Object(slider), false);
    }
    g_top = 0;
    g_hung = true;
}

void Slide(u64 system, u32 mode) {
    if (system != g_system || !g_hung || Read<s32>(system + SystemCategory) != CameraCategory ||
        (mode != SlideIn && mode != SlideOut)) {
        return;
    }
    for (const Slider& slider : g_sliders) {
        const u64 row = Object(slider);
        const u64 handle = Read<u64>(row + RowHandle);
        if (mode == SlideIn) {
            PlayTrack(handle, SlideTrack, Selected(row) ? SelectedAnimation : InAnimation);
            PlayTrack(handle, KnobTrack, InAnimation);
        } else {
            PlayTrack(handle, SlideTrack, OutAnimation);
            PlayTrack(handle, KnobTrack, OutAnimation);
        }
    }
}

void Draw(u64 system, u64 context, DrawFunction draw) {
    if (system != g_system || !g_hung || Read<s32>(system + SystemCategory) != CameraCategory) {
        return;
    }
    for (const Slider& slider : g_sliders) {
        Core::Recompiled::Call(draw, Read<u64>(Object(slider) + RowHandle), context);
    }
}

void End(u64 system) {
    if (system != g_system || g_system == 0) {
        return;
    }
    for (Slider& slider : g_sliders) {
        if (!slider.made) {
            continue;
        }
        if (!Loaded(slider)) {
            g_broken = true;
            continue;
        }
        Call<RowFunction>(EndSlider)(Object(slider));
        slider.made = false;
    }
    g_hung = false;
    g_system = 0;
}

} // namespace Gr2Port::CameraMenu
