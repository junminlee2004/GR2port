// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL_events.h>

#include "common/logging/log.h"
#include "core/language/language.h"

#include "graphics/gr2port/exit_rows.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::ExitRows {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// u64 FUN_015165c0(texts, key): the record of a text key in the language's bank, or 0.
constexpr Hooks::Entry FindText{0x15165c0, 6, Hooks::FramePrologue};
// void FUN_01243cb0(float width, float, float, button, asset, key, selected key, selected, ,
// disabled): makes a menu button and starts its clip's load. Width is the widest text of the
// list's rows, which sets the length of each of them.
constexpr Hooks::Entry MakeButton{0x1243cb0, 6, Hooks::FramePrologue};
// void FUN_01244330(button): a menu button's frame.
constexpr Hooks::Entry TickButton{0x1244330, 6, Hooks::FramePrologue};
// void FUN_01395b50(handle, context): draws a clip.
constexpr Hooks::Entry DrawClip{0x1395b50, 6, Hooks::FramePrologue};
// void FUN_012f21b0(page) and FUN_012f1e70(page): the Save / Load page's frame and its end.
constexpr Hooks::Entry UpdatePage{0x12f21b0, 6, Hooks::FramePrologue};
constexpr Hooks::Entry EndPage{0x12f1e70, 6, Hooks::FramePrologue};
// bool FUN_0179f9e0(menu) and FUN_0179ee60(menu): the frame and the end of the title menu that
// has the two stories.
constexpr Hooks::Entry UpdateTitle{0x179f9e0, 6, Hooks::FramePrologue};
constexpr Hooks::Entry EndTitle{0x179ee60, 6, Hooks::FramePrologue};
// u64 FUN_01396ec0(input): reads the pad into a menu's input. The title menu's frame starts with
// it; the pause menu reads its pages' input before their frames.
constexpr Hooks::Entry ReadInput{0x1396ec0, 6, Hooks::FramePrologueR14};

// FUN_013775e0 makes that title menu and its four rows.
constexpr VAddr TitleMakerStart = 0x13775e0;
constexpr VAddr TitleMakerEnd = 0x13775e0 + 5208;

// void FUN_01244810(button): slides a hidden button in.
constexpr VAddr SlideIn = 0x1244810;
// void FUN_01244920(button, char selected)
constexpr VAddr Select = 0x1244920;
// void FUN_013e0e20(float speed, float start, clip, u32 node, u32 animation)
constexpr VAddr Play = 0x13e0e20;
// void FUN_01394ed0(handle): ends a clip handle, which the heap then frees.
constexpr VAddr EndClip = 0x1394ed0;
constexpr VAddr HeapFree = 0x17f56b0;
constexpr VAddr MutexLock = 0x17f6000;
constexpr VAddr MutexUnlock = 0x17f6010;
// void FUN_011e09b0(key, int kind, int choice): opens a dialog; kind 1 has Yes and No.
constexpr VAddr OpenDialog = 0x11e09b0;
// void FUN_014e0ea0(float volume, u32 sound, 0)
constexpr VAddr PlaySound = 0x14e0ea0;
// void FUN_0150cbd0(float* width, float* height, style, key): a text's size in the default style.
constexpr VAddr MeasureText = 0x150cbd0;
// void FUN_012f31e0(page): slides the page's rows and background out.
constexpr VAddr LeavePage = 0x12f31e0;
// void FUN_01395e30(clip, u32 node, const float* offset): moves a clip's node.
constexpr VAddr MoveNode = 0x1395e30;
// entry FUN_013967a0(handle, u32 node, u32 index): the text a clip handle shows at a node, made
// when the node has fewer than index + 1 of them. FUN_01390fb0(text, key) sets its key.
constexpr VAddr AttachText = 0x13967a0;
constexpr VAddr SetTextKey = 0x1390fb0;

// The text manager, the heap of the interface, and the list of live interface objects with its
// mutex: a load that ends after its object left the list does not reach the object.
constexpr VAddr Texts = 0x1f270a0;
constexpr VAddr Heap = 0x1cdf970;
constexpr VAddr LiveObjects = 0x1ee2c88;
constexpr u64 LiveFirst = 0x08;
constexpr u64 LiveLast = 0x10;
constexpr u64 LiveMutex = 0x20;
// The open dialog's answer: -1 while it is open, then 0 for Yes and 1 for No.
constexpr VAddr Dialog = 0x1eed678;
constexpr u64 DialogAnswer = 0x10;
// The pause menu, which holds the input its pages read.
constexpr VAddr PauseMenu = 0x1eee178;
constexpr u64 PauseMenuInput = 0xd0;
// The bit numbers of the buttons that decide and cancel.
constexpr VAddr DecideButton = 0x1c81b70;
constexpr VAddr CancelButton = 0x1c81b74;
// The code of the language the game shows, such as "EN" or "BP".
constexpr VAddr LanguageCode = 0x1cdf800;

// A menu's input: the buttons pressed this frame, the ones that repeat while held, and whether
// the menu reads them.
constexpr u64 InputPressed = 0x08;
constexpr u64 InputRepeated = 0x80;
constexpr u64 InputEnabled = 0x18c;
constexpr u32 OptionsButton = 0x4000;
constexpr u32 UpButtons = 0x800400;
constexpr u32 DownButtons = 0x1001000;

// The Save / Load page: its state, its three rows and the count of rows it slid in.
constexpr u64 PageState = 0x08;
constexpr u64 PageRows = 0x18;
constexpr u64 PageStarted = 0x30;
constexpr s32 PageOpening = 1;
constexpr s32 PageInput = 2;
// The page waits for its rows to slide out, then opens what the selected row names.
constexpr s32 PageDecided = 3;
// The page waits for the answer about the title: Yes goes to the title, No brings the rows back.
constexpr s32 PageAsking = 7;
constexpr u32 PageDecideSound = 0x8bed3ae5;
constexpr u32 PageUpSound = 0xda16c583;
constexpr u32 PageDownSound = 0x33f3f50e;

// The title menu: its input, the handle of the clip that places the rows, the four rows
// (Continue, Load Game, New Game, Credits), the nodes of the row places, the node of the
// captions, the state, the selected row and the distance a list moves down per row it lacks.
constexpr u64 TitleInput = 0x90;
constexpr u64 TitleLayout = 0xa0;
constexpr u64 TitleRows = 0xf8;
constexpr u64 TitlePlaces = 0x13c;
constexpr u64 TitleCaptionNode = 0x154;
constexpr u64 TitleState = 0x16c;
constexpr u64 TitleSelected = 0x174;
constexpr u64 TitleRowDrop = 0x18c;
// The list slides in, then takes input; it reads the input in both states.
constexpr s32 TitleOpening = 0xc;
constexpr s32 TitleListed = 0xd;
constexpr size_t TitleRowCount = 4;
// The game ticks and draws New Game for either story.
constexpr size_t TitleAnchorRow = 2;
// The selected row's number while the added row is selected: the menu shows the caption of that
// number, and reads its rows by the number only on input.
constexpr s32 TitleOwnRow = 4;
constexpr u32 TitleDecideSound = 0xd8755a48;
constexpr u32 TitleUpSound = 0x031abbf8;
constexpr u32 TitleDownSound = 0x7afe0797;

constexpr u32 NoNode = 0x8cde0001;

// A menu button, 0x20 bytes that FUN_01243cb0 fills.
struct Button {
    u64 handle;
    float length;
    u32 root;
    u32 text;
    u32 selected_text;
    s32 state;
    u8 selected;
    u8 pressed;
    u8 flag;
    u8 disabled;
};
static_assert(sizeof(Button) == 0x20);

constexpr s32 ButtonLoading = 0;
constexpr s32 ButtonSlidingIn = 1;
constexpr s32 ButtonShown = 2;
constexpr s32 ButtonPressed = 3;
constexpr s32 ButtonSlidingOut = 4;
constexpr s32 ButtonHidden = 5;

// A button clip's tracks: the slide (in, idle, out), the look (selected, selected idle,
// unselected, unselected idle, pressed), the length and the press.
constexpr size_t SlideTrack = 0;
constexpr size_t SlideOut = 2;
constexpr size_t LookTrack = 1;
constexpr size_t LookSelected = 1;
constexpr size_t LookPressed = 4;
constexpr size_t PressTrack = 3;

// One track of a clip handle: the index of its animation that plays, its node and its animations.
struct Track {
    u32 current;
    u32 node;
    u8 reversed;
    u8 unused[15];
    const u32* first;
    const u32* last;
    u64 capacity;
};
static_assert(sizeof(Track) == 0x30);

// A clip handle: the clip, its tracks and the texts attached to its nodes.
constexpr u64 HandleClip = 0x00;
constexpr u64 HandleFirstTrack = 0x10;
constexpr u64 HandleLastTrack = 0x18;
constexpr u64 HandleFirstText = 0x30;
constexpr u64 HandleLastText = 0x38;

// An attached text: its node, its number among the node's texts, then the text, which has its
// scale and whether it is hidden.
struct AttachedText {
    u32 node;
    u32 index;
    u64 text;
};
static_assert(sizeof(AttachedText) == 0x10);
constexpr u64 TextScale = 0x120;
constexpr u64 TextHidden = 0x160;

// A clip: the node it hangs from, and its own nodes.
constexpr u64 ClipParent = 0xb0;
constexpr u64 ClipNodes = 0x100;

// A clip's node. A clip that hangs from a node takes the node's world matrix, four rows of four
// floats with the translation last, and, in a draw that asks for it, the node's opacity.
struct alignas(32) Node {
    u8 head[0x80];
    std::array<float, 16> world;
    u8 pad[8];
    float opacity;
    u8 tail[0x14];
};
static_assert(sizeof(Node) == 0xe0);

using FindTextFunction = u64 PS4_SYSV_ABI (*)(u64 texts, const char* key);
using MakeButtonFunction = void PS4_SYSV_ABI (*)(float width, float left, float right,
                                                 Button* button, const char* asset, const char* key,
                                                 const char* selected_key, u8 selected, u8 flag,
                                                 u8 disabled);
using ButtonFunction = void PS4_SYSV_ABI (*)(Button* button);
using SelectFunction = void PS4_SYSV_ABI (*)(Button* button, char selected);
using DrawFunction = void PS4_SYSV_ABI (*)(u64 handle, u64 context);
using ObjectFunction = u64 PS4_SYSV_ABI (*)(u64 object);
using PlayFunction = void PS4_SYSV_ABI (*)(float speed, float start, u64 clip, u32 node,
                                           u32 animation);
using HeapFreeFunction = void PS4_SYSV_ABI (*)(u64 heap, u64 block);
using DialogFunction = void PS4_SYSV_ABI (*)(const char* key, s32 kind, s32 choice);
using SoundFunction = void PS4_SYSV_ABI (*)(float volume, u32 sound, u64 unused);
using MeasureFunction = void PS4_SYSV_ABI (*)(float* width, float* height, u64 style,
                                              const char* key);
using MoveNodeFunction = void PS4_SYSV_ABI (*)(u64 clip, u32 node, const float* offset);
using AttachTextFunction = u64 PS4_SYSV_ABI (*)(u64 handle, u32 node, u32 index);
using SetTextKeyFunction = void PS4_SYSV_ABI (*)(u64 text, const char* key);

enum class Phase {
    Idle,
    // The Save / Load page slides its rows out before it asks.
    Leaving,
    Asking,
    Quitting,
};

// A row's texts: the key the game looks up, the key of the game's text whose record gives the
// style, and the English text.
struct Text {
    const char* key;
    const char* model;
    const char* english;
};

constexpr char KeyPrefix[] = "gr2port_exit_";
constexpr char PageKeys[] = "ui_saveload_selects_1_";
constexpr char CaptionKey[] = "gr2port_exit_caption";
constexpr char QuestionKey[] = "gr2port_exit_ask";

constexpr std::array Records{
    Text{"gr2port_exit_page_1", "ui_saveload_selects_1_3", "Exit to Desktop"},
    Text{"gr2port_exit_page_2", "ui_saveload_selects_2_3", "Exit to Desktop"},
    Text{"gr2port_exit_title_1", "ui_tt_menu_selects_07", "EXIT TO DESKTOP"},
    Text{"gr2port_exit_title_2", "ui_tt_menu_selects_08", "EXIT TO DESKTOP"},
    Text{CaptionKey, "ui_tt_menu_caption_02", "Close the game and return to the desktop."},
    Text{QuestionKey, "ui_menu_yesno_system_2", "Are you sure you want to quit to desktop?"},
};

struct Row {
    // The keys of the row's text and of its text while selected.
    std::array<const char*, 2> keys;
    // The key of the list's last row, which the game makes right before this one.
    const char* last_key;
    // The dialog's first choice, as the menu's own question has it.
    s32 choice;
    Button button{};
    // The node the row's clip hangs from.
    Node place{};
    bool made = false;
    bool captioned = false;
    Phase phase = Phase::Idle;
    // The game's row this one is ticked and drawn after.
    const Button* anchor = nullptr;
    // The place is base's, moved by the step from one node to the next.
    const Node* base = nullptr;
    const Node* from = nullptr;
    const Node* to = nullptr;
};

VAddr g_base = 0;
FindTextFunction g_find_text = nullptr;
MakeButtonFunction g_make_button = nullptr;
ButtonFunction g_tick_button = nullptr;
DrawFunction g_draw_clip = nullptr;
ObjectFunction g_update_page = nullptr;
ObjectFunction g_end_page = nullptr;
ObjectFunction g_update_title = nullptr;
ObjectFunction g_end_title = nullptr;
ObjectFunction g_read_input = nullptr;
// The title menu while its frame runs.
u64 g_title_menu = 0;

Row g_page{{"gr2port_exit_page_1", "gr2port_exit_page_2"}, "ui_saveload_selects_1_3", -1};
Row g_title{{"gr2port_exit_title_1", "gr2port_exit_title_2"}, "ui_tt_menu_selects_07", 1};

std::mutex g_records_mutex;
std::array<std::vector<u8>, Records.size()> g_records;

VAddr Game(VAddr address) {
    return g_base + address - GoldHenBase;
}

template <typename T>
T Read(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

template <typename T>
void Write(VAddr address, T value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(T));
}

template <typename Function>
Function Call(VAddr address) {
    return reinterpret_cast<Function>(Game(address));
}

// A text record of the game's bank: a 0x1c byte header (the count of characters, the count of
// bytes, the count of attributes, then the style), the UTF-8 text and its terminator padded to 4
// bytes, then the attributes, 8 bytes each: a first character, an end character and four bytes
// of style.
constexpr size_t RecordHeader = 0x1c;
constexpr size_t RecordAttribute = 8;

// The record of text in the style of the game's record model.
std::vector<u8> MakeRecord(u64 model, std::string_view text) {
    const u16 characters = static_cast<u16>(
        std::ranges::count_if(text, [](char byte) { return (byte & 0xc0) != 0x80; }));
    const u16 bytes = static_cast<u16>(text.size());
    const s16 attributes = Read<s16>(model + 4) > 0 ? 1 : 0;
    const size_t attribute_offset = RecordHeader + ((text.size() + 4) & ~size_t{3});
    std::vector<u8> record(attribute_offset + attributes * RecordAttribute);
    std::memcpy(record.data(), reinterpret_cast<const void*>(model), RecordHeader);
    std::memcpy(record.data(), &characters, sizeof(characters));
    std::memcpy(record.data() + 2, &bytes, sizeof(bytes));
    std::memcpy(record.data() + 4, &attributes, sizeof(attributes));
    std::memcpy(record.data() + RecordHeader, text.data(), text.size());
    if (attributes != 0) {
        const u64 model_attribute = model + RecordHeader + ((Read<u16>(model + 2) + 4) & ~3);
        u8* const attribute = record.data() + attribute_offset;
        std::memcpy(attribute, reinterpret_cast<const void*>(model_attribute), RecordAttribute);
        const u16 first = 0;
        std::memcpy(attribute, &first, sizeof(first));
        std::memcpy(attribute + 2, &characters, sizeof(characters));
    }
    return record;
}

// The console language of each language the game has text in, but its two Englishes.
struct GameLanguage {
    const char* code;
    int console_language;
};
constexpr std::array GameLanguages{
    GameLanguage{"JP", 0},  GameLanguage{"FR", 2},  GameLanguage{"ES", 3},  GameLanguage{"DE", 4},
    GameLanguage{"IT", 5},  GameLanguage{"NL", 6},  GameLanguage{"PT", 7},  GameLanguage{"RU", 8},
    GameLanguage{"KR", 9},  GameLanguage{"CT", 10}, GameLanguage{"CS", 11}, GameLanguage{"PL", 16},
    GameLanguage{"BP", 17}, GameLanguage{"MX", 20}, GameLanguage{"CA", 22},
};

// The rows are in the language the game shows, which is not always the console's: a game that
// lacks the console's language shows English or another regional form.
std::string Shown(const char* english) {
    const char* const code = Read<const char*>(Game(LanguageCode));
    if (code != nullptr) {
        for (const GameLanguage& language : GameLanguages) {
            if (std::strcmp(code, language.code) == 0) {
                return Language::TextIn(language.console_language, english);
            }
        }
    }
    return english;
}

u64 PS4_SYSV_ABI OnFindText(u64 texts, const char* key) {
    if (key == nullptr || std::strncmp(key, KeyPrefix, sizeof(KeyPrefix) - 1) != 0) {
        return g_find_text(texts, key);
    }
    for (size_t i = 0; i < Records.size(); ++i) {
        if (std::strcmp(key, Records[i].key) != 0) {
            continue;
        }
        std::scoped_lock lock{g_records_mutex};
        if (g_records[i].empty()) {
            const u64 model = g_find_text(texts, Records[i].model);
            if (model == 0) {
                return 0;
            }
            g_records[i] = MakeRecord(model, Shown(Records[i].english));
        }
        return reinterpret_cast<u64>(g_records[i].data());
    }
    return g_find_text(texts, key);
}

bool IsShown(const Button& button) {
    return button.state == ButtonSlidingIn || button.state == ButtonShown;
}

// Starts an animation of a clip handle's track.
void PlayTrack(u64 handle, size_t track_index, size_t animation) {
    Track* const first = Read<Track*>(handle + HandleFirstTrack);
    const Track* const last = Read<const Track*>(handle + HandleLastTrack);
    if (static_cast<size_t>(last - first) <= track_index) {
        return;
    }
    Track& track = first[track_index];
    if (static_cast<size_t>(track.last - track.first) <= animation) {
        return;
    }
    Call<PlayFunction>(Play)(1.0f / 30.0f, 0.0f, Read<u64>(handle + HandleClip), track.node,
                             track.first[animation]);
    track.reversed = 0;
    track.current = static_cast<u32>(animation);
}

void Slide(Button& button) {
    if (IsShown(button)) {
        PlayTrack(button.handle, SlideTrack, SlideOut);
        button.state = ButtonSlidingOut;
    }
}

// The pressed look of a selected row; the button's frame returns it to the shown state.
void Press(Button& button) {
    if (IsShown(button) && button.selected != 0 && button.pressed == 0) {
        PlayTrack(button.handle, PressTrack, 0);
        PlayTrack(button.handle, LookTrack, LookPressed);
        button.pressed = 1;
        button.state = ButtonPressed;
    }
}

void Release(Button& button) {
    PlayTrack(button.handle, LookTrack, LookSelected);
    button.pressed = 0;
}

void SetSelected(Button* button, bool selected) {
    Call<SelectFunction>(Select)(button, selected ? 1 : 0);
}

void Sound(u32 sound) {
    Call<SoundFunction>(PlaySound)(1.0f, sound, 0);
}

void Ask(Row& row) {
    Call<DialogFunction>(OpenDialog)(QuestionKey, 1, row.choice);
    row.phase = Phase::Asking;
}

s32 Answer() {
    return Read<s32>(Read<u64>(Game(Dialog)) + DialogAnswer);
}

void Quit() {
    SDL_Event event{};
    event.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&event);
}

u32 ButtonBit(VAddr number) {
    const u32 bit = Read<u32>(Game(number));
    return bit < 0x1b ? 1u << bit : 0;
}

// The node a button's clip hangs from.
const Node* PlaceOf(const Button& button) {
    const u64 clip = button.handle != 0 ? Read<u64>(button.handle + HandleClip) : 0;
    return clip != 0 ? Read<const Node*>(clip + ClipParent) : nullptr;
}

// Hangs the row's clip one row below the game's last row.
void Place(Row& row) {
    const u64 clip = Read<u64>(row.button.handle + HandleClip);
    if (row.base == nullptr || clip == 0) {
        return;
    }
    row.place.world = row.base->world;
    row.place.opacity = row.base->opacity;
    for (size_t i = 12; i < 15; ++i) {
        row.place.world[i] += row.to->world[i] - row.from->world[i];
    }
    Write<const Node*>(clip + ClipParent, &row.place);
}

void End(Row& row) {
    if (!row.made) {
        return;
    }
    const u64 live = Read<u64>(Game(LiveObjects));
    if (live != 0) {
        Call<ObjectFunction>(MutexLock)(live + LiveMutex);
        u64* const first = Read<u64*>(live + LiveFirst);
        u64* const last = Read<u64*>(live + LiveLast);
        Write<u64*>(live + LiveLast, std::remove(first, last, reinterpret_cast<u64>(&row.button)));
        Call<ObjectFunction>(MutexUnlock)(live + LiveMutex);
    }
    if (row.button.handle != 0) {
        Call<ObjectFunction>(EndClip)(row.button.handle);
        Call<HeapFreeFunction>(HeapFree)(Read<u64>(Game(Heap)), row.button.handle);
    }
    row.button = {};
    row.made = false;
    row.captioned = false;
    row.phase = Phase::Idle;
    row.anchor = nullptr;
    row.base = nullptr;
}

// Runs a menu's frame; with masked, the menu reads no input in it.
u64 Update(ObjectFunction update, u64 menu, u64 input, bool masked) {
    if (!masked) {
        return update(menu);
    }
    const u8 enabled = Read<u8>(input + InputEnabled);
    Write<u8>(input + InputEnabled, 0);
    const u64 result = update(menu);
    Write<u8>(input + InputEnabled, enabled);
    return result;
}

void PS4_SYSV_ABI OnMakeButton(float width, float left, float right, Button* button,
                               const char* asset, const char* key, const char* selected_key,
                               u8 selected, u8 flag, u8 disabled) {
    const VAddr caller = reinterpret_cast<VAddr>(__builtin_return_address(0));
    Row* row = nullptr;
    if (std::strncmp(key, PageKeys, sizeof(PageKeys) - 1) == 0) {
        row = &g_page;
    } else if (caller >= Game(TitleMakerStart) && caller < Game(TitleMakerEnd)) {
        row = &g_title;
    }
    if (row == nullptr) {
        g_make_button(width, left, right, button, asset, key, selected_key, selected, flag,
                      disabled);
        return;
    }
    // The list's rows are as long as its widest text needs, the added row's among them.
    float own_width = 0.0f;
    Call<MeasureFunction>(MeasureText)(&own_width, nullptr, 0, row->keys[0]);
    width = std::max(width, own_width);
    g_make_button(width, left, right, button, asset, key, selected_key, selected, flag, disabled);
    if (std::strcmp(key, row->last_key) == 0) {
        End(*row);
        g_make_button(width, left, right, &row->button, asset, row->keys[0], row->keys[1], 0, 0, 0);
        row->made = true;
    }
}

void PS4_SYSV_ABI OnTickButton(Button* button) {
    g_tick_button(button);
    for (Row* const row : {&g_page, &g_title}) {
        if (row->made && button == row->anchor) {
            Place(*row);
            g_tick_button(&row->button);
        }
    }
}

void PS4_SYSV_ABI OnDrawClip(u64 handle, u64 context) {
    g_draw_clip(handle, context);
    for (const Row* const row : {&g_page, &g_title}) {
        if (row->made && row->anchor != nullptr && handle == row->anchor->handle &&
            row->button.state != ButtonLoading && row->button.state != ButtonHidden) {
            g_draw_clip(row->button.handle, context);
        }
    }
}

u64 PS4_SYSV_ABI OnUpdatePage(u64 page) {
    Row& row = g_page;
    if (!row.made) {
        return g_update_page(page);
    }
    const std::array<Button*, 3> rows = Read<std::array<Button*, 3>>(page + PageRows);
    row.anchor = rows[2];
    row.from = PlaceOf(*rows[1]);
    row.to = PlaceOf(*rows[2]);
    row.base = row.from != nullptr ? row.to : nullptr;
    if (row.phase == Phase::Asking) {
        const s32 answer = Answer();
        if (answer == 0) {
            Quit();
            row.phase = Phase::Quitting;
        } else if (answer == 1) {
            // The page's own frame brings the rows back.
            row.phase = Phase::Idle;
        }
    }
    if (row.phase == Phase::Quitting) {
        // The page's own frame takes Yes as the answer about the title.
        return 0;
    }
    const u64 input = Read<u64>(Read<u64>(Game(PauseMenu)) + PauseMenuInput);
    // With none of its rows selected, the page refuses a decision.
    bool masked = row.button.selected != 0 && !IsShown(row.button);
    if (Read<s32>(page + PageState) == PageInput && row.phase == Phase::Idle &&
        IsShown(row.button) && Read<u8>(input + InputEnabled) != 0) {
        const u32 pressed = Read<u32>(input + InputPressed);
        const u32 repeated = Read<u32>(input + InputRepeated);
        const bool decide = (pressed & ButtonBit(DecideButton)) != 0;
        const bool up = (repeated & UpButtons) != 0;
        const bool down = (repeated & DownButtons) != 0;
        if ((pressed & (OptionsButton | ButtonBit(CancelButton))) != 0) {
            // The page closes.
        } else if (row.button.selected != 0) {
            if (decide) {
                Sound(PageDecideSound);
                Call<ObjectFunction>(LeavePage)(page);
                Write<s32>(page + PageState, PageDecided);
                Slide(row.button);
                row.phase = Phase::Leaving;
            } else if (up) {
                SetSelected(&row.button, false);
                SetSelected(rows[2], true);
                Sound(PageUpSound);
                masked = true;
            } else if (down) {
                // With none of its rows selected, the page selects its first.
                SetSelected(&row.button, false);
            }
        } else if (!decide) {
            if (up) {
                if (rows[0]->selected != 0 && rows[1]->selected == 0 && rows[2]->selected == 0) {
                    SetSelected(rows[0], false);
                    SetSelected(&row.button, true);
                    Sound(PageUpSound);
                    masked = true;
                }
            } else if (down && rows[2]->selected != 0) {
                SetSelected(rows[2], false);
                SetSelected(&row.button, true);
                Sound(PageDownSound);
                masked = true;
            }
        }
    }
    const u64 result = Update(g_update_page, page, input, masked);
    const bool hidden = std::ranges::all_of(
        rows, [](const Button* button) { return button->state == ButtonHidden; });
    const s32 state = Read<s32>(page + PageState);
    if (row.phase == Phase::Leaving && state == PageDecided && hidden &&
        row.button.state == ButtonHidden) {
        Ask(row);
        Write<s32>(page + PageState, PageAsking);
    }
    if (std::ranges::any_of(
            rows, [](const Button* button) { return button->state == ButtonSlidingOut; })) {
        Slide(row.button);
    }
    if (row.button.state == ButtonHidden && row.phase == Phase::Idle && row.base != nullptr &&
        (state == PageOpening || state == PageInput) && Read<s32>(page + PageStarted) == 3) {
        if (std::ranges::any_of(rows, [](const Button* button) { return button->selected != 0; })) {
            SetSelected(&row.button, false);
        }
        Call<ButtonFunction>(SlideIn)(&row.button);
    }
    return result;
}

u64 PS4_SYSV_ABI OnEndPage(u64 page) {
    End(g_page);
    return g_end_page(page);
}

// Adds the row's caption to the title menu's captions, hidden as the menu makes its own.
void AddCaption(u64 menu) {
    const u64 layout = Read<u64>(menu + TitleLayout);
    const u32 node = Read<u32>(menu + TitleCaptionNode);
    const u64 made = Call<AttachTextFunction>(AttachText)(layout, node, TitleOwnRow);
    if (made == 0) {
        return;
    }
    Call<SetTextKeyFunction>(SetTextKey)(Read<AttachedText>(made).text, CaptionKey);
    const AttachedText* const last = Read<const AttachedText*>(layout + HandleLastText);
    for (const AttachedText* text = Read<const AttachedText*>(layout + HandleFirstText);
         text != last; ++text) {
        if (text->node == node && text->index == TitleOwnRow) {
            Write<std::array<float, 4>>(text->text + TextScale, {1.0f, 1.0f, 1.0f, 0.0f});
            Write<u8>(text->text + TextHidden, 1);
            break;
        }
    }
}

// Places the row below the last row of the list the title menu shows, and slides it in.
void ShowTitleRow(Row& row, u64 menu, const std::array<Button*, TitleRowCount>& rows) {
    const Button* last = nullptr;
    u32 missing = 0;
    for (const Button* const button : rows) {
        if (IsShown(*button)) {
            last = button;
        } else {
            ++missing;
        }
    }
    const u64 layout = Read<u64>(Read<u64>(menu + TitleLayout) + HandleClip);
    const std::array<u32, 2> places = Read<std::array<u32, 2>>(menu + TitlePlaces);
    if (last == nullptr || layout == 0 || places[0] == NoNode || places[1] == NoNode) {
        return;
    }
    const Node* const nodes = Read<const Node*>(layout + ClipNodes);
    row.base = PlaceOf(*last);
    row.from = nodes + places[0];
    row.to = nodes + places[1];
    if (row.base == nullptr) {
        return;
    }
    // The menu moves a list's rows down by a distance for each row the story lacks.
    alignas(16) const std::array<float, 4> drop{
        0.0f, static_cast<float>(missing) * Read<float>(menu + TitleRowDrop), 0.0f, 0.0f};
    if (row.button.root != NoNode) {
        Call<MoveNodeFunction>(MoveNode)(Read<u64>(row.button.handle + HandleClip), row.button.root,
                                         drop.data());
    }
    if (!row.captioned) {
        AddCaption(menu);
        row.captioned = true;
    }
    SetSelected(&row.button, false);
    Call<ButtonFunction>(SlideIn)(&row.button);
}

// Moves the title menu's selection onto the row and off it, and asks when the row is chosen. It
// runs when the menu has read the pad, before the menu's own input code.
void SteerTitle(Row& row, u64 menu, u64 input) {
    const auto rows = Read<std::array<Button*, TitleRowCount>>(menu + TitleRows);
    const s32 state = Read<s32>(menu + TitleState);
    const u32 pressed = Read<u32>(input + InputPressed);
    const u32 repeated = Read<u32>(input + InputRepeated);
    const bool decide = (pressed & ButtonBit(DecideButton)) != 0;
    const bool cancel = (pressed & ButtonBit(CancelButton)) != 0;
    const bool down = (repeated & DownButtons) != 0;
    const bool up = (repeated & UpButtons) != 0;
    const s32 selected = Read<s32>(menu + TitleSelected);
    const bool listed = state == TitleOpening || state == TitleListed;
    // The menu takes no input while the question is open. With the added row selected the list
    // takes only the button that closes it: its other input code reads its rows by the selected
    // number, which stays the added row's until the list opens again.
    bool masked =
        row.phase != Phase::Idle || (listed && selected == TitleOwnRow && (decide || !cancel));
    if (listed && row.phase == Phase::Idle && IsShown(row.button) &&
        Read<u8>(input + InputEnabled) != 0) {
        s32 first = -1;
        s32 last = -1;
        for (s32 i = 0; i < static_cast<s32>(TitleRowCount); ++i) {
            if (IsShown(*rows[i])) {
                first = first < 0 ? i : first;
                last = i;
            }
        }
        const auto select = [&](s32 from, s32 to, u32 sound) {
            SetSelected(from == TitleOwnRow ? &row.button : rows[from], false);
            Write<s32>(menu + TitleSelected, to);
            SetSelected(to == TitleOwnRow ? &row.button : rows[to], true);
            Sound(sound);
        };
        if (first < 0) {
            // The list has no row to move between.
        } else if (selected == TitleOwnRow) {
            if (decide) {
                Sound(TitleDecideSound);
                Press(row.button);
                Ask(row);
            } else if (cancel) {
                // The list closes.
            } else if (down) {
                select(TitleOwnRow, first, TitleDownSound);
            } else if (up) {
                select(TitleOwnRow, last, TitleUpSound);
            }
        } else if (!decide && !cancel) {
            if (down) {
                if (selected == last) {
                    select(selected, TitleOwnRow, TitleDownSound);
                    masked = true;
                }
            } else if (up && selected == first) {
                select(selected, TitleOwnRow, TitleUpSound);
                masked = true;
            }
        }
    }
    if (masked) {
        Write<u32>(input + InputPressed, 0);
        Write<u32>(input + InputRepeated, 0);
    }
}

u64 PS4_SYSV_ABI OnReadInput(u64 input) {
    const u64 result = g_read_input(input);
    if (g_title_menu != 0 && input == Read<u64>(g_title_menu + TitleInput)) {
        SteerTitle(g_title, g_title_menu, input);
    }
    return result;
}

u64 PS4_SYSV_ABI OnUpdateTitle(u64 menu) {
    Row& row = g_title;
    if (!row.made) {
        return g_update_title(menu);
    }
    const auto rows = Read<std::array<Button*, TitleRowCount>>(menu + TitleRows);
    row.anchor = rows[TitleAnchorRow];
    if (row.phase == Phase::Asking) {
        const s32 answer = Answer();
        if (answer == 0) {
            Quit();
            row.phase = Phase::Quitting;
        } else if (answer == 1) {
            Release(row.button);
            row.phase = Phase::Idle;
        }
    }
    g_title_menu = menu;
    const u64 result = g_update_title(menu);
    g_title_menu = 0;
    if (std::ranges::any_of(
            rows, [](const Button* button) { return button->state == ButtonSlidingOut; })) {
        Slide(row.button);
    }
    if (row.button.state == ButtonHidden && row.phase == Phase::Idle &&
        Read<s32>(menu + TitleState) == TitleListed) {
        ShowTitleRow(row, menu, rows);
    }
    return result;
}

u64 PS4_SYSV_ABI OnEndTitle(u64 menu) {
    End(g_title);
    return g_end_title(menu);
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
    g_base = module_base;
    if (!Replace(module_base, FindText, c, capacity, &OnFindText, g_find_text) ||
        !Replace(module_base, MakeButton, c, capacity, &OnMakeButton, g_make_button) ||
        !Replace(module_base, TickButton, c, capacity, &OnTickButton, g_tick_button) ||
        !Replace(module_base, DrawClip, c, capacity, &OnDrawClip, g_draw_clip) ||
        !Replace(module_base, UpdatePage, c, capacity, &OnUpdatePage, g_update_page) ||
        !Replace(module_base, EndPage, c, capacity, &OnEndPage, g_end_page) ||
        !Replace(module_base, UpdateTitle, c, capacity, &OnUpdateTitle, g_update_title) ||
        !Replace(module_base, EndTitle, c, capacity, &OnEndTitle, g_end_title) ||
        !Replace(module_base, ReadInput, c, capacity, &OnReadInput, g_read_input)) {
        LOG_ERROR(Render_Vulkan, "GR2port: a menu function is not the expected code");
        return false;
    }
    LOG_INFO(Render_Vulkan, "GR2port: the menus have an Exit to Desktop row");
    return true;
}

} // namespace Gr2Port::ExitRows
