// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/menu_kit.h"
#include "graphics/gr2port/photo_ui.h"
#include "graphics/gr2port/ui_items.h"

namespace Gr2Port::PhotoUi {

namespace {

using namespace Xbyak::util;

// A byte that is set while the camera is out, from the press that takes it out to the press that
// puts it away (HUD.isPhotoMode, FUN_011b0f90, reads it).
constexpr VAddr CameraOutByte = 0x1ee3105;

// void FUN_011aa4a0(camera): photo mode's frame on the game's main thread. The camera holds the
// filter the player picked, which the shot hands to the photo's draw.
constexpr Hooks::Entry CameraFrame{0x11aa4a0, 6, Hooks::FramePrologue};
constexpr u64 CameraFilter = 0x3628;

// void FUN_011ba250(list): the control list's frame, which hands each of its slots the row the
// slot asks for. FUN_011bad80, the list's tick each frame, calls it while a slot asks for another
// row than it shows.
constexpr Hooks::Entry ListFrame{0x11ba250, 6, Hooks::FramePrologue};
constexpr Hooks::Entry ListTick{0x11bad80, 6, Hooks::FramePrologueR14};
// The rows the list's slots ask for, a pointer or null for each. Slots 6 to 12 run down the left
// column a row apart, and slot 13 lies at its foot, where the Collection row shows. Slot 11 holds
// the Filter Effect row; slot 10 above it and slot 12 below it are empty.
constexpr u64 SlotRows = 0x80;
constexpr u32 FilterSlot = 11;
constexpr u32 HideSlot = 12;
constexpr u32 RotateSlot = 10;
// The list's slots, a pointer to each. A slot holds its state, 3 and more once its clip loaded
// and hangs from the slot's place, the clip, and the row the clip shows.
constexpr u64 ListSlots = 0x140;
constexpr u64 SlotState = 0x0c;
constexpr u64 SlotClip = 0x20;
constexpr u64 SlotRow = 0x28;
constexpr s32 SlotLoaded = 3;

// A row of the control list, as the game's own rows are.
struct ListRow {
    // The game's numbers of the buttons whose presses play the row's press. The first must be a
    // button: the list reads its press time for every row it shows. The second may be -1.
    s32 button;
    s32 second_button;
    // The row's text in the game's table of them, or -1 for the text of key.
    s32 text;
    // 0 when the row draws the Filter Effect header above itself.
    s32 header;
    const char* key;
    // The icon's textures: the small one and the one the row shows.
    const char* small_icon;
    const char* icon;
};
static_assert(sizeof(ListRow) == 0x28);

// The game's L3 and SHARE icons, which no small icons of their own come with. None of the game's
// texts and rows shows them, so the button prompts draw Hide UI's buttons, L3 + R2, or its key in
// the first, and Rotate Camera's button with the right stick, or its key, in the second.
constexpr char HideIcon[] = "photomode_key_19";
constexpr char RotateIcon[] = "photomode_key_23";
// The game's text whose record gives the rows' texts their style: Put Away Camera's, in the same
// column.
constexpr char ModelKey[] = "ui_camera_guide_06";
// The game's number of L3, the first button of Hide UI and the button of Rotate Camera.
constexpr s32 L3Number = 8;

// The rows GR2port adds, Hide UI and Rotate Camera, the slot each one takes and its English text.
// Rotate Camera's row shows one row below Hide UI's, where the list has no slot: its clip hangs
// from a place of its own there.
constexpr std::array<ListRow, 2> Rows{{
    {L3Number, -1, -1, -1, "gr2port_photo_hide", HideIcon, HideIcon},
    {L3Number, -1, -1, -1, "gr2port_photo_rotate", RotateIcon, RotateIcon},
}};
constexpr std::array<u32, Rows.size()> RowSlots{HideSlot, RotateSlot};
constexpr std::array<const char*, Rows.size()> English{"Hide UI", "Rotate Camera"};

// void FUN_0171aa10(window): the gesture window's frame on the game's main thread, each frame,
// which adds the window's draw items for the draw thread to run.
constexpr Hooks::Entry GestureDraw{0x171aa10, 6, Hooks::FramePrologue};
using GestureDrawFunction = void PS4_SYSV_ABI (*)(void* window);
// The pools the game takes each frame's draw items from: the pool in use, each pool's memory, and
// how many of its bytes are taken.
constexpr VAddr ItemPool = 0x1f21bac;
constexpr VAddr ItemPools = 0x1f21bf0;
constexpr VAddr ItemPoolTaken = 0x1f21c08;
constexpr size_t ItemPoolCount = 3;
// The draw thread's call of a draw item's function: mov rdi, rbx; call [rax + 0x10]. rax holds the
// item's virtual table.
constexpr Hooks::Entry ItemCall{0x84d1fc, 6, {0x48, 0x89, 0xdf, 0xff, 0x50, 0x10}};
constexpr VAddr ItemCallNext = 0x84d202;

std::atomic<VAddr> g_base{0};
std::atomic<VAddr> g_camera{0};
std::atomic<bool> g_hidden{false};
std::mutex g_record_mutex;
std::array<std::vector<u8>, Rows.size()> g_records;
GestureDrawFunction g_gesture_draw = nullptr;
// The draw items of the gesture window's last frame in each pool.
struct ItemRange {
    std::atomic<VAddr> first{0};
    std::atomic<VAddr> end{0};
};
std::array<ItemRange, ItemPoolCount> g_gesture_items;
// The place Rotate Camera's row hangs from.
MenuKit::Node g_rotate_place{};

// The loaded clip of the list's slot, or 0; with row, only while the slot shows that row.
VAddr SlotClipOf(VAddr list, u32 slot, const ListRow* row = nullptr) {
    const VAddr object =
        MenuKit::Read<VAddr>(MenuKit::Read<VAddr>(list + ListSlots) + slot * sizeof(VAddr));
    if (object == 0 || MenuKit::Read<s32>(object + SlotState) < SlotLoaded ||
        (row != nullptr && MenuKit::Read<const ListRow*>(object + SlotRow) != row)) {
        return 0;
    }
    return MenuKit::Read<VAddr>(object + SlotClip);
}

// Hangs Rotate Camera's row one row below Hide UI's: from Hide UI's place, moved by the step from
// the Filter Effect row's place to it. The places move as the list slides in and out.
void PlaceRotateRow(VAddr list) {
    const VAddr rotate = SlotClipOf(list, RotateSlot, &Rows[1]);
    const VAddr hide = SlotClipOf(list, HideSlot, &Rows[0]);
    const VAddr filter = SlotClipOf(list, FilterSlot);
    if (rotate == 0 || hide == 0 || filter == 0) {
        return;
    }
    const auto* const above = MenuKit::Read<const MenuKit::Node*>(filter + MenuKit::ClipParent);
    const auto* const place = MenuKit::Read<const MenuKit::Node*>(hide + MenuKit::ClipParent);
    if (above == nullptr || place == nullptr) {
        return;
    }
    g_rotate_place = *place;
    for (size_t axis = 12; axis < 15; ++axis) {
        g_rotate_place.world[axis] += place->world[axis] - above->world[axis];
    }
    MenuKit::Write<const MenuKit::Node*>(rotate + MenuKit::ClipParent, &g_rotate_place);
}

// Before the list's frame: asks for the added rows while there is a Filter Effect row, and takes
// them out once there is none.
void PS4_SYSV_ABI OnListFrame(u32, const Hooks::Registers* registers, u64) {
    const VAddr list = registers->rdi;
    const VAddr slots = list + SlotRows;
    const auto* filter = MenuKit::Read<const ListRow*>(slots + FilterSlot * sizeof(VAddr));
    const bool wanted = filter != nullptr && filter->header == 0;
    for (u32 i = 0; i < Rows.size(); ++i) {
        const VAddr slot = slots + RowSlots[i] * sizeof(VAddr);
        const auto* row = MenuKit::Read<const ListRow*>(slot);
        if (wanted && row == nullptr) {
            MenuKit::Write(slot, &Rows[i]);
        } else if (!wanted && row == &Rows[i]) {
            MenuKit::Write<const ListRow*>(slot, nullptr);
        }
    }
}

void PS4_SYSV_ABI OnListTick(u32, const Hooks::Registers* registers, u64) {
    PlaceRotateRow(registers->rdi);
}

void PS4_SYSV_ABI OnCameraFrame(u32, const Hooks::Registers* registers, u64) {
    g_camera.store(registers->rdi, std::memory_order_relaxed);
}

// Notes which draw items are the gesture window's.
void PS4_SYSV_ABI DrawGesture(void* window) {
    const u32 pool = MenuKit::Read<u32>(MenuKit::Game(ItemPool));
    const VAddr memory = MenuKit::Read<VAddr>(MenuKit::Game(ItemPools) + u64{pool} * sizeof(VAddr));
    const VAddr taken = MenuKit::Game(ItemPoolTaken) + u64{pool} * sizeof(u32);
    const VAddr first = memory + MenuKit::Read<u32>(taken);
    Core::Recompiled::Call(g_gesture_draw, window);
    g_gesture_items[pool].first.store(first, std::memory_order_relaxed);
    g_gesture_items[pool].end.store(memory + MenuKit::Read<u32>(taken), std::memory_order_relaxed);
}

// In place of the draw thread's call of a draw item: while the screen drawing is hidden, the
// gesture window's items keep their draws.
void PS4_SYSV_ABI RunItem(VAddr item) {
    const bool kept = g_hidden.load(std::memory_order_relaxed) &&
                      std::ranges::any_of(g_gesture_items, [item](const ItemRange& range) {
                          return item >= range.first.load(std::memory_order_relaxed) &&
                                 item < range.end.load(std::memory_order_relaxed);
                      });
    UiItems::RunItem(item, kept);
}

} // namespace

void Install(VAddr module_base) {
    g_base.store(module_base, std::memory_order_relaxed);
}

bool CameraOut() {
    const VAddr base = g_base.load(std::memory_order_relaxed);
    return base != 0 && *Hooks::Code(base, CameraOutByte) != 0;
}

bool WatchCamera(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    if (!Hooks::Hooked(
            Hooks::ObserveEntry(module_base, CameraFrame, c, capacity, &OnCameraFrame, 0))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the photo mode's frame is not the expected code");
        return false;
    }
    return true;
}

u32 Filter() {
    const VAddr camera = g_camera.load(std::memory_order_relaxed);
    if (!CameraOut() || camera == 0) {
        return Filters;
    }
    return std::min(MenuKit::Read<u32>(camera + CameraFilter), Filters);
}

void ToggleHidden() {
    if (CameraOut()) {
        g_hidden.store(!g_hidden.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
}

void OnPadRead() {
    if (!CameraOut()) {
        g_hidden.store(false, std::memory_order_relaxed);
    }
}

bool AddRow(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    if (!Hooks::Hooked(Hooks::ObserveEntry(module_base, ListFrame, c, capacity, &OnListFrame, 0)) ||
        !Hooks::Hooked(Hooks::ObserveEntry(module_base, ListTick, c, capacity, &OnListTick, 0))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the photo mode's control list is not the expected code");
        return false;
    }
    const void* gesture_draw = nullptr;
    if (!Hooks::Hooked(Hooks::Replace(module_base, GestureDraw, c, capacity,
                                      reinterpret_cast<const void*>(&DrawGesture),
                                      &gesture_draw))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the gesture window's drawing is not the expected code");
        return false;
    }
    g_gesture_draw = reinterpret_cast<GestureDrawFunction>(const_cast<void*>(gesture_draw));
    return true;
}

bool RouteItems(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    const void* const item_call = c.getCurr();
    c.mov(rdi, rbx);
    c.mov(rax, reinterpret_cast<u64>(&RunItem));
    c.call(rax);
    c.jmp(Hooks::Code(module_base, ItemCallNext), Xbyak::CodeGenerator::T_NEAR);
    if (Hooks::Replace(module_base, ItemCall, c, capacity, item_call) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the draw thread's item call is not the expected code");
        return false;
    }
    return true;
}

u64 FindText(u64 texts, const char* key, GraphicsMenu::FindTextFunction find) {
    const auto row = std::ranges::find_if(
        Rows, [key](const ListRow& listed) { return std::strcmp(key, listed.key) == 0; });
    if (row == Rows.end()) {
        return Core::Recompiled::Call(find, texts, key);
    }
    std::scoped_lock lock{g_record_mutex};
    std::vector<u8>& record = g_records[row - Rows.begin()];
    if (record.empty()) {
        const u64 model = Core::Recompiled::Call(find, texts, ModelKey);
        if (model == 0) {
            return 0;
        }
        record = MenuKit::MakeRecord(model, MenuKit::Shown(English[row - Rows.begin()]));
    }
    return reinterpret_cast<u64>(record.data());
}

bool Hidden() {
    return g_hidden.load(std::memory_order_relaxed);
}

void ForgetItems(u32 context) {
    g_gesture_items[context].first.store(0, std::memory_order_relaxed);
    g_gesture_items[context].end.store(0, std::memory_order_relaxed);
}

} // namespace Gr2Port::PhotoUi
