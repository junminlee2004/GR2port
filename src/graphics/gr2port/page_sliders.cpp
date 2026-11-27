// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <span>
#include <string>
#include <thread>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/low_energy_alert.h"
#include "graphics/gr2port/menu_kit.h"
#include "graphics/gr2port/page_sliders.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/special_cameras.h"
#include "graphics/gr2port/ui_capture.h"

namespace Gr2Port::PageSliders {

namespace {

using namespace MenuKit;

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
// The object every System page shares, and the category it shows.
constexpr VAddr SystemObject = 0x1eee210;
constexpr u64 SystemCategory = 0x10c;
// The game's names: the slider clip and its caption's text slot.
constexpr VAddr SliderAsset = 0x18f4970;
constexpr VAddr Text01 = 0x18f7194;
// The class of a System Menu slider.
constexpr VAddr SliderClass = 0x1ae8370;

// A page's state: it enters, slides in, takes input, plays the press of Reset to Defaults, slides
// out.
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

// A slider a page takes: its caption, the setting that holds it, and how much of the setting one
// step of the slider is.
struct Added {
    const char* caption;
    u32& (*setting)(Settings::Values& values);
    u32 step;
};
// The most sliders a page has of its own and takes, and the most rows of a page.
constexpr size_t MostOwn = 3;
constexpr size_t MostAdded = 2;
constexpr size_t MostRows = 7;
// The settings of a page's added sliders.
using Amounts = std::array<u32, MostAdded>;

// A page that takes sliders. It lists them with its own sliders, above or below them, and shows
// as many of the listed sliders as it has sliders of its own, in their places.
struct Page {
    // void (page): the page's frame.
    Hooks::Entry frame;
    // The category the System object shows on the page, where the object holds the page's rows,
    // and how many there are.
    s32 category;
    u64 rows;
    size_t row_count;
    // The rows of the page's own sliders, from the top.
    std::span<const size_t> own;
    std::span<const Added> added;
    bool added_above;
    // The rows the cursor comes onto the list from: by Down onto its top, by Up onto its bottom.
    // Off an added slider at that end of the list, the cursor goes to the first of them.
    std::span<const size_t> above;
    std::span<const size_t> below;
    // Puts the settings of the added sliders to use.
    void (*apply)(const Amounts& amounts);
    const char* name;
};

// FUN_01307eb0, the Camera Configuration page. Its rows: the two pairs of buttons of the vertical
// and horizontal controls, Reset to Defaults, then the sliders Motion Sensitivity and Camera
// Sensitivity, which stand between the second pair and Reset to Defaults. The added sliders hold
// the camera sensitivities in percent of the game's own.
constexpr std::array<size_t, 2> CameraSliders{5, 6};
constexpr std::array<size_t, 2> CameraAbove{2, 3};
constexpr std::array<size_t, 1> CameraBelow{4};
constexpr std::array<Added, 2> CameraAdded{{
    {"Special Attack Camera Sensitivity",
     [](Settings::Values& values) -> u32& { return values.mouse.special_attack_camera; }, 2},
    {"Panther Mode Camera Sensitivity",
     [](Settings::Values& values) -> u32& { return values.mouse.panther_camera; }, 2},
}};
constexpr Page CameraPage{
    .frame = {0x1307eb0, 6, Hooks::FramePrologue},
    .category = 3,
    .rows = 0x90,
    .row_count = 7,
    .own = CameraSliders,
    .added = CameraAdded,
    .added_above = false,
    .above = CameraAbove,
    .below = CameraBelow,
    .apply = [](const Amounts& amounts) { SpecialCameras::Set(amounts[0], amounts[1]); },
    .name = "Camera Configuration",
};

// FUN_01313d60, the Sound page. Its rows: the sliders Music Volume and SFX Volume, Reset to
// Defaults, the ON and OFF buttons of the low volume setting, then the slider Controller Speaker
// Volume. The sliders stand above the buttons. Reset to Defaults is the lowest row, and the row the
// cursor goes to by Up from the highest.
constexpr std::array<size_t, 3> SoundSliders{0, 1, 5};
constexpr std::array<size_t, 1> SoundAbove{2};
constexpr std::array<size_t, 2> SoundBelow{3, 4};
constexpr std::array<Added, 1> SoundAdded{{
    {"Low Energy Alert",
     [](Settings::Values& values) -> u32& { return values.game.low_energy_alert; }, 1},
}};
constexpr Page SoundPage{
    .frame = {0x1313d60, 6, Hooks::FramePrologue},
    .category = 4,
    .rows = 0xb0,
    .row_count = 6,
    .own = SoundSliders,
    .added = SoundAdded,
    .added_above = true,
    .above = SoundAbove,
    .below = SoundBelow,
    .apply = [](const Amounts& amounts) { LowEnergyAlert::Set(amounts[0]); },
    .name = "Sound",
};

// A layout unit is 100 canvas pixels, from the canvas's middle, with y upwards. The scroll bar
// stands beside the sliders on screen: its middle BarRight canvas pixels right of theirs, from
// BarAbove above the first place, over its caption, to BarBelow below the last, under its knob.
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

// A page and its list of sliders.
struct Listing {
    const Page* page;
    // The page's own frame, once it is hooked.
    PageFunction frame = nullptr;
    std::array<Slider, MostAdded> sliders;
    // Once the sliders hang on the page: the page's rows, the listed sliders and the places of
    // the page's own sliders.
    std::array<u64, MostRows> rows{};
    std::array<u64, MostOwn + MostAdded> list{};
    std::array<u64, MostOwn> places{};
    // The first listed slider on screen.
    int top = 0;
    // Where the scroll bar's thumb is, from 0 at the top to 1 at the bottom, or -1 until the page
    // shows it, and when it last moved.
    float thumb = -1.0f;
    std::chrono::steady_clock::time_point thumb_time;
    // The frames Right and Left were held, up to FastRepeats.
    u32 right = 0;
    u32 left = 0;
    // The settings as the page opened.
    Amounts opened{};
    std::atomic<u64> saves{0};

    int OnScreen() const {
        return static_cast<int>(page->own.size());
    }
    int Listed() const {
        return static_cast<int>(page->own.size() + page->added.size());
    }
    bool IsAdded(int index) const {
        return page->added_above ? index < static_cast<int>(page->added.size())
                                 : index >= OnScreen();
    }
    std::span<Slider> Sliders() {
        return std::span{sliders}.first(page->added.size());
    }
    std::span<const Slider> Sliders() const {
        return std::span{sliders}.first(page->added.size());
    }
};
static_assert(CameraPage.own.size() <= MostOwn && CameraPage.added.size() <= MostAdded &&
              CameraPage.row_count <= MostRows);
static_assert(SoundPage.own.size() <= MostOwn && SoundPage.added.size() <= MostAdded &&
              SoundPage.row_count <= MostRows);

std::array<Listing, 2> g_listings{{{&CameraPage}, {&SoundPage}}};
// The System object the sliders belong to, and whether they hang on its pages.
u64 g_system = 0;
bool g_hung = false;
// A slider that never loaded keeps its memory until the game exits, and the pages stay as the
// game made them.
bool g_broken = false;

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
GameFunction<Function> Method(u64 row, u64 slot) {
    return {reinterpret_cast<Function>(Read<u64>(Read<u64>(row) + slot))};
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

Amounts AmountsOf(const Listing& listing) {
    Amounts amounts{};
    for (size_t i = 0; i < listing.page->added.size(); ++i) {
        amounts[i] = static_cast<u32>(Read<s32>(Object(listing.sliders[i]) + SliderValue)) *
                     listing.page->added[i].step;
    }
    return amounts;
}

void Apply(const Listing& listing) {
    listing.page->apply(AmountsOf(listing));
}

// Writes a page's settings to config.json on a thread of its own; a later save of the page writes
// them all.
void Save(Listing& listing, const Amounts& amounts) {
    const u64 number = listing.saves.fetch_add(1) + 1;
    std::thread([&listing, amounts, number] {
        if (listing.saves.load() != number) {
            return;
        }
        if (Settings::Update([&](Settings::Values& stored) {
                for (size_t i = 0; i < listing.page->added.size(); ++i) {
                    listing.page->added[i].setting(stored) = amounts[i];
                }
            })) {
            LOG_INFO(Render_Vulkan, "GR2port: the settings of the {} page's sliders are saved",
                     listing.page->name);
        }
    }).detach();
}

// Makes a slider as the System Menu makes its own, at value.
bool Make(Slider& slider, const Added& added, s32 value) {
    const VAddr object = Object(slider);
    slider.object = {};
    slider.key = std::string{KeyPrefix} + added.caption;
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

// Hangs the listed sliders from the first one on screen in the page's places, and hides the
// others.
void ShowList(const Listing& listing) {
    for (int i = 0; i < listing.Listed(); ++i) {
        const bool on_screen = i >= listing.top && i < listing.top + listing.OnScreen();
        if (on_screen) {
            HangFrom(listing.list[i], listing.places[i - listing.top]);
        }
        Show(listing.list[i], on_screen);
    }
}

// The listed slider the cursor is on, or the number of listed sliders.
int Cursor(const Listing& listing) {
    for (int i = listing.top; i < listing.top + listing.OnScreen(); ++i) {
        if (Selected(listing.list[i])) {
            return i;
        }
    }
    return listing.Listed();
}

void Move(Listing& listing, int from, int to) {
    Select(listing.list[from], false);
    const int top = std::clamp(listing.top, to - listing.OnScreen() + 1, to);
    if (top != listing.top) {
        listing.top = top;
        ShowList(listing);
    }
    Select(listing.list[to], true);
    Sound(to > from ? DownSound : UpSound);
}

// Steps an added slider's value as the page steps its own: by 1, by FastStep once its button was
// held for FastRepeats frames, and around to the other end on a fresh press at an end.
void Step(const Listing& listing, u64 slider, s32 sign, u32 held) {
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
    Apply(listing);
    Sound(sign > 0 ? UpSound : DownSound);
}

// The cursor comes onto the list's top or bottom, the listed slider to, from one of the rows from,
// and the list shows that end. The page moves the cursor onto a slider of its own; true when the
// cursor went onto an added one here.
bool Enter(Listing& listing, int to, std::span<const size_t> from, u32 sound) {
    const int top = to == 0 ? 0 : listing.Listed() - listing.OnScreen();
    if (top != listing.top) {
        listing.top = top;
        ShowList(listing);
    }
    if (!listing.IsAdded(to)) {
        return false;
    }
    for (const size_t row : from) {
        if (Selected(listing.rows[row])) {
            Select(listing.rows[row], false);
        }
    }
    Select(listing.list[to], true);
    Sound(sound);
    return true;
}

// The cursor goes off the added slider at, an end of the list, to row.
void Leave(const Listing& listing, int at, size_t row, u32 sound) {
    Select(listing.list[at], false);
    Select(listing.rows[row], true);
    Sound(sound);
}

// Moves the cursor through the listed sliders and steps the added ones ahead of the page's own
// input code, in the page's order of Down, Left, Up and Right. True when the page must not see
// this frame's input.
bool Steer(Listing& listing, u64 input) {
    if (Read<u8>(input + InputEnabled) == 0) {
        return false;
    }
    const u32 pressed = Read<u32>(input + InputPressed);
    const u32 held = Read<u32>(input + InputHeld);
    const u32 repeated = Read<u32>(input + InputRepeated);
    const bool horizontal = (held & LeftButtons) == 0 || (held & RightButtons) == 0;
    if (horizontal) {
        listing.right = (held & RightButtons) != 0 ? std::min(listing.right + 1, FastRepeats) : 0;
        listing.left = (held & LeftButtons) != 0 ? std::min(listing.left + 1, FastRepeats) : 0;
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
    const Page& page = *listing.page;
    const auto selected = [&listing](size_t row) { return Selected(listing.rows[row]); };
    const int last = listing.Listed() - 1;
    const int at = Cursor(listing);
    if (at > last) {
        if (down && std::ranges::any_of(page.above, selected)) {
            return Enter(listing, 0, page.above, DownSound);
        }
        if (!down && up && std::ranges::any_of(page.below, selected)) {
            return Enter(listing, last, page.below, UpSound);
        }
        return false;
    }
    if (down) {
        if (at < last) {
            Move(listing, at, at + 1);
        } else if (listing.IsAdded(at)) {
            Leave(listing, at, page.below.front(), DownSound);
        } else {
            return false;
        }
        return true;
    }
    if (!listing.IsAdded(at)) {
        // The page steps its own sliders and moves the cursor up off the first listed.
        if (!left && up && at > 0) {
            Move(listing, at, at - 1);
            return true;
        }
        return false;
    }
    if (left) {
        Step(listing, listing.list[at], -1, listing.left);
    } else if (up && at > 0) {
        Move(listing, at, at - 1);
    } else if (up) {
        Leave(listing, at, page.above.front(), UpSound);
    } else if (right) {
        Step(listing, listing.list[at], 1, listing.right);
    }
    return true;
}

void Open(Listing& listing) {
    for (const Slider& slider : listing.Sliders()) {
        if (Selected(Object(slider))) {
            Select(Object(slider), false);
        }
    }
    listing.top = 0;
    ShowList(listing);
    listing.thumb = -1.0f;
    listing.right = 0;
    listing.left = 0;
    listing.opened = AmountsOf(listing);
}

// Shows where the sliders on screen lie among the listed ones, on a bar beside them that comes and
// goes with them. The System Menu's list hangs from the same places, and fades them in again as the
// page goes.
void ShowBar(Listing& listing) {
    const Node& first = *reinterpret_cast<const Node*>(listing.places.front());
    const Node& last = *reinterpret_cast<const Node*>(listing.places[listing.OnScreen() - 1]);
    const auto now = std::chrono::steady_clock::now();
    const float wanted =
        static_cast<float>(listing.top) / static_cast<float>(listing.Listed() - listing.OnScreen());
    if (listing.thumb < 0.0f) {
        listing.thumb = wanted;
    } else {
        const float seconds = std::chrono::duration<float>(now - listing.thumb_time).count();
        listing.thumb += (wanted - listing.thumb) * (1.0f - std::exp(-seconds / ThumbTime));
    }
    listing.thumb_time = now;
    const float top = 0.5f * CanvasHeight - first.world[13] * UnitPixels - BarAbove;
    const float bottom = 0.5f * CanvasHeight - last.world[13] * UnitPixels + BarBelow;
    const float thumb = (bottom - top) * listing.OnScreen() / listing.Listed();
    const float thumb_top = top + (bottom - top - thumb) * listing.thumb;
    UiCapture::SetScrollBar({
        .x = 0.5f * CanvasWidth + first.world[12] * UnitPixels + BarRight,
        .width = BarWidth,
        .top = top,
        .bottom = bottom,
        .thumb_top = thumb_top,
        .thumb_bottom = thumb_top + thumb,
        .opacity = Read<s32>(g_system + SystemCategory) == listing.page->category
                       ? std::clamp(first.opacity, 0.0f, 1.0f)
                       : 0.0f,
    });
}

void OnPage(Listing& listing, u64 page) {
    if (!g_hung || Read<u64>(Game(SystemObject)) != g_system) {
        Core::Recompiled::Call(listing.frame, page);
        return;
    }
    const u64 input = Read<u64>(Read<u64>(Game(PauseMenu)) + PauseMenuInput);
    const s32 state = Read<s32>(page + PageState);
    if (state == PageEnter) {
        Open(listing);
    }
    if (state == PageInput && Steer(listing, input)) {
        const u8 enabled = Read<u8>(input + InputEnabled);
        Write<u8>(input + InputEnabled, 0);
        Core::Recompiled::Call(listing.frame, page);
        Write<u8>(input + InputEnabled, enabled);
        // The page's input code sets the repeats' pace from its own counts.
        if (std::max(listing.right, listing.left) >= FastRepeats) {
            Write<u32>(input + InputRepeatFrames, FastFrames);
        }
    } else {
        Core::Recompiled::Call(listing.frame, page);
    }
    const s32 next = Read<s32>(page + PageState);
    if (state == PageInput && next == PageReset) {
        for (const Slider& slider : listing.Sliders()) {
            Write<s32>(Object(slider) + SliderValue, SliderDefault);
        }
        Apply(listing);
    } else if (state == PageInput && next == PageClosing) {
        if (const Amounts amounts = AmountsOf(listing); amounts != listing.opened) {
            Save(listing, amounts);
        }
    }
    for (const Slider& slider : listing.Sliders()) {
        Method<RowFunction>(Object(slider), TickSlot)(Object(slider));
    }
    ShowBar(listing);
}

void PS4_SYSV_ABI OnCameraPage(u64 page) {
    OnPage(g_listings[0], page);
}

void PS4_SYSV_ABI OnSoundPage(u64 page) {
    OnPage(g_listings[1], page);
}

// The listing of the page that system shows, once the sliders hang on it, or null.
Listing* Shown(u64 system) {
    if (system != g_system || !g_hung) {
        return nullptr;
    }
    const s32 category = Read<s32>(system + SystemCategory);
    const auto shown = std::ranges::find_if(g_listings, [category](const Listing& listing) {
        return listing.frame != nullptr && listing.page->category == category;
    });
    return shown != g_listings.end() ? &*shown : nullptr;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    constexpr std::array<PageFunction, 2> frames{&OnCameraPage, &OnSoundPage};
    bool hooked = true;
    for (size_t i = 0; i < g_listings.size(); ++i) {
        Listing& listing = g_listings[i];
        const void* trampoline = nullptr;
        if (capacity - c.getSize() < Hooks::MaxStubBytes ||
            !Hooks::Holds(module_base, listing.page->frame) ||
            Hooks::Replace(module_base, listing.page->frame, c, capacity,
                           reinterpret_cast<const void*>(frames[i]),
                           &trampoline) != Hooks::Result::Hooked) {
            LOG_ERROR(Render_Vulkan, "GR2port: the {} page is not the expected code",
                      listing.page->name);
            hooked = false;
            continue;
        }
        listing.frame = reinterpret_cast<PageFunction>(const_cast<void*>(trampoline));
    }
    return hooked;
}

void MakeRows(u64 system) {
    if (g_broken) {
        return;
    }
    Settings::Values values = Settings::Get();
    bool made = false;
    for (Listing& listing : g_listings) {
        if (listing.frame == nullptr) {
            continue;
        }
        for (size_t i = 0; i < listing.page->added.size(); ++i) {
            const Added& added = listing.page->added[i];
            const s32 value = static_cast<s32>(added.setting(values) / added.step);
            if (!Make(listing.sliders[i], added, value)) {
                LOG_ERROR(Render_Vulkan, "GR2port: the {} page's sliders could not be made",
                          listing.page->name);
                g_broken = true;
                return;
            }
        }
        made = true;
    }
    if (made) {
        g_system = system;
    }
}

bool RowsLoaded() {
    return g_system == 0 || g_broken || std::ranges::all_of(g_listings, [](const Listing& listing) {
               return listing.frame == nullptr || std::ranges::all_of(listing.Sliders(), &Loaded);
           });
}

void Hang(u64 system) {
    if (system != g_system || g_broken || g_hung) {
        return;
    }
    for (Listing& listing : g_listings) {
        if (listing.frame == nullptr) {
            continue;
        }
        const Page& page = *listing.page;
        const u64 rows = Read<u64>(system + page.rows);
        for (size_t i = 0; i < page.row_count; ++i) {
            listing.rows[i] = Read<u64>(rows + i * sizeof(u64));
        }
        size_t listed = page.added_above ? page.added.size() : 0;
        for (size_t i = 0; i < page.own.size(); ++i) {
            const u64 row = listing.rows[page.own[i]];
            listing.list[listed++] = row;
            listing.places[i] = Read<u64>(ClipOf(row) + ClipParent);
        }
        listed = page.added_above ? 0 : listed;
        for (const Slider& slider : listing.Sliders()) {
            listing.list[listed++] = Object(slider);
            HangFrom(Object(slider), listing.places[page.own.size() - 1]);
            Show(Object(slider), false);
        }
        listing.top = 0;
    }
    g_hung = true;
}

void BeforeSlide(u64 system, u32 mode) {
    Listing* const listing = Shown(system);
    if (listing == nullptr || mode != SlideIn) {
        return;
    }
    // The page showed every row of its own and put the cursor on its first.
    ShowList(*listing);
    if (!listing->IsAdded(0)) {
        return;
    }
    for (const size_t own : listing->page->own) {
        if (Selected(listing->rows[own])) {
            Select(listing->rows[own], false);
        }
    }
    Select(listing->list[0], true);
}

void Slide(u64 system, u32 mode) {
    const Listing* const listing = Shown(system);
    if (listing == nullptr || (mode != SlideIn && mode != SlideOut)) {
        return;
    }
    for (const Slider& slider : listing->Sliders()) {
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
    const Listing* const listing = Shown(system);
    if (listing == nullptr) {
        return;
    }
    for (const Slider& slider : listing->Sliders()) {
        Core::Recompiled::Call(draw, Read<u64>(Object(slider) + RowHandle), context);
    }
}

void End(u64 system) {
    if (system != g_system || g_system == 0) {
        return;
    }
    for (Listing& listing : g_listings) {
        for (Slider& slider : listing.Sliders()) {
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
    }
    g_hung = false;
    g_system = 0;
}

} // namespace Gr2Port::PageSliders
