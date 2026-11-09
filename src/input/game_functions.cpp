// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <SDL3/SDL_timer.h>

#include "common/logging/log.h"
#include "core/libraries/pad/pad.h"
#include "input/controller.h"
#include "input/game_functions.h"
#include "input/input_mouse.h"

namespace Input::GameFunctions {

namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;

constexpr u32 TypeA = 0;
constexpr u32 TypeB = 1;
constexpr u32 TypeC = 2;
constexpr u32 TypeD = 3;

constexpr std::array<std::string_view, TypeCount> Same(std::string_view pad) {
    return {pad, pad, pad, pad, pad};
}

// FUN_015cc610 registers the player's actions for a control type. Every type puts Dodge and the
// style change on R2 or the touch pad, and the other actions on the same buttons. The touch pad
// flicks of Type B to Type E evade in the flicked direction; Type B cycles the styles with R2;
// Type C to Type E change style with R2 and up or down on the left stick, the right stick or the
// face buttons, and R2 alone goes back to Normal.
constexpr Function Table[]{
    {"gr2_move_forward", "Move Forward", Same("Left stick up"), "axis_left_y_minus", "w"},
    {"gr2_move_back", "Move Back", Same("Left stick down"), "axis_left_y_plus", "s"},
    {"gr2_move_left", "Move Left", Same("Left stick left"), "axis_left_x_minus", "a"},
    {"gr2_move_right", "Move Right", Same("Left stick right"), "axis_left_x_plus", "d"},
    {"gr2_jump", "Jump", Same("Cross"), "cross", "space"},
    {"gr2_attack", "Normal Attack", Same("Square"), "square", "leftbutton"},
    {"gr2_special_attack", "Special Attack", Same("Triangle"), "triangle", "f"},
    {"gr2_stasis_field", "Stasis Field (Grab)", Same("Circle"), "circle", "r"},
    {"gr2_shift_gravity", "Shift Gravity", Same("R1"), "r1", "rightbutton"},
    {"gr2_return_gravity", "Return Gravity", Same("L1"), "l1", "q"},
    {"gr2_gravity_slide", "Gravity Slide (Hold)", Same("L2"), "l2", "lshift"},
    {"gr2_evade",
     "Evade",
     {"R2", "Flick the touch pad", "Flick the touch pad", "Flick the touch pad",
      "Flick the touch pad"},
     "",
     "e"},
    {"gr2_lunar_style",
     "Lunar Style",
     {"Flick the touch pad up", "R2 until Lunar", "R2 + left stick up", "R2 + right stick up",
      "R2 + triangle"},
     "",
     "mousewheelup"},
    {"gr2_jupiter_style",
     "Jupiter Style",
     {"Flick the touch pad down", "R2 until Jupiter", "R2 + left stick down",
      "R2 + right stick down", "R2 + cross"},
     "",
     "mousewheeldown"},
    {"gr2_normal_style",
     "Normal Style",
     {"Tap the touch pad", "R2 until Normal", "R2", "R2", "R2"},
     "",
     "middlebutton"},
    {"gr2_panther", "Panther Mode (Hold)", Same("Hold the touch pad"), "touchpad_center", "c"},
    {"gr2_reset_camera", "Reset Camera", Same("R3"), "r3", ""},
    {"gr2_photo_mode", "Enter Photo Mode", Same("D-pad up"), "pad_up", "1"},
    {"gr2_map", "Display the Map", Same("D-pad right"), "pad_right", "2"},
    {"gr2_gesture", "Initiate Gesture", Same("D-pad down"), "pad_down", "3"},
    {"gr2_first_person", "First-person View", Same("D-pad left"), "pad_left", "4"},
    {"gr2_pause", "Pause", Same("OPTIONS"), "options", "escape"},
    {"gr2_zoom_in", "Zoom In (Photo Mode)", Same("R2"), "r2", ""},
    {"gr2_flick_up", "Flick Up (Touch Pad)", Same("Flick the touch pad up"), "touchpad_swipe_up",
     ""},
    {"gr2_flick_down", "Flick Down (Touch Pad)", Same("Flick the touch pad down"),
     "touchpad_swipe_down", ""},
    {"gr2_flick_left", "Flick Left (Touch Pad)", Same("Flick the touch pad left"),
     "touchpad_swipe_left", "z"},
    {"gr2_flick_right", "Flick Right (Touch Pad)", Same("Flick the touch pad right"),
     "touchpad_swipe_right", "x"},
};

std::atomic<u32> g_type{TypeA};
// -1 until the game reports it.
std::atomic<s32> g_style{-1};

// Type B: each R2 press moves to the next style, so a style key taps R2 until the player has the
// style, at most twice. A later press stops the taps of an earlier one.
constexpr u32 TapMs = 80;
constexpr u32 SettleMs = 150;
std::atomic<u32> g_cycle{0};

struct Cycle {
    GameController* controller;
    u32 id;
    Style target;
    u32 taps_left;
    bool down;
};

// Type C to Type E: R2 goes down with the key, the stick tilt or face button a moment later, so
// the game sees R2 held first. A release stops a tilt that has not started yet.
constexpr u32 ChordDelayMs = 50;
std::mutex g_chord_mutex;
u32 g_chord = 0;

struct Chord {
    GameController* controller;
    u32 id;
    u32 type;
    Style style;
};

void PressR2(GameController* controller, bool pressed) {
    controller->Axis(Axis::TriggerRight, pressed ? 255 : 0);
    controller->Button(OrbisPadButtonDataOffset::R2, pressed);
}

int StickValue(s16 position) {
    return GetAxis(-0x80, 0x7f, position);
}

// The stick tilt or face button that picks Lunar or Jupiter with R2 held.
void PressStyleInput(GameController* controller, u32 type, Style style, bool pressed,
                     s16 left_y = 0, s16 right_y = 0) {
    const bool up = style == Style::Lunar;
    switch (type) {
    case TypeC:
        controller->Axis(Axis::LeftY, pressed ? (up ? 0 : 255) : StickValue(left_y));
        break;
    case TypeD:
        controller->Axis(Axis::RightY, pressed ? (up ? 0 : 255) : StickValue(right_y));
        break;
    default:
        controller->Button(
            up ? OrbisPadButtonDataOffset::Triangle : OrbisPadButtonDataOffset::Cross, pressed);
        break;
    }
}

Uint32 SDLCALL CycleStep(void* data, SDL_TimerID, Uint32) {
    Cycle* const cycle = static_cast<Cycle*>(data);
    if (cycle->down) {
        PressR2(cycle->controller, false);
        cycle->down = false;
        return SettleMs;
    }
    if (g_cycle.load() != cycle->id || cycle->taps_left == 0 ||
        g_style.load() == static_cast<s32>(cycle->target)) {
        delete cycle;
        return 0;
    }
    PressR2(cycle->controller, true);
    cycle->down = true;
    --cycle->taps_left;
    return TapMs;
}

Uint32 SDLCALL ChordStep(void* data, SDL_TimerID, Uint32) {
    const std::unique_ptr<Chord> chord{static_cast<Chord*>(data)};
    std::scoped_lock lock{g_chord_mutex};
    if (g_chord == chord->id) {
        PressStyleInput(chord->controller, chord->type, chord->style, true);
    }
    return 0;
}

} // namespace

std::span<const Function> Functions() {
    return Table;
}

const Function* Find(std::string_view name) {
    const auto it = std::ranges::find(Table, name, &Function::name);
    return it != std::end(Table) ? &*it : nullptr;
}

u32 ControlType() {
    return g_type.load();
}

void SetGameState(u32 type, Style style) {
    if (type < TypeCount) {
        g_type.store(type);
    }
    g_style.store(static_cast<s32>(style));
}

void SetControlType(u32 type) {
    if (type < TypeCount) {
        g_type.store(type);
    }
}

void Evade(GameController* controller, bool pressed, s16 left_x, s16 left_y) {
    if (g_type.load() == TypeA) {
        PressR2(controller, pressed);
        return;
    }
    if (!pressed) {
        return;
    }
    // Toward the stick's larger tilt; back when it is centred.
    int direction = BUTTON_SWIPE_DOWN;
    if (std::abs(left_x) > std::abs(left_y)) {
        direction = left_x > 0 ? BUTTON_SWIPE_RIGHT : BUTTON_SWIPE_LEFT;
    } else if (left_y < 0) {
        direction = BUTTON_SWIPE_UP;
    }
    TriggerButtonSwipe(controller, direction);
}

void ChangeStyle(GameController* controller, Style style, bool pressed, s16 left_y, s16 right_y) {
    const u32 type = g_type.load();
    if (type == TypeA) {
        switch (style) {
        case Style::Normal:
            controller->SetTouchpadState(0, pressed, 0.5f, 0.5f);
            controller->Button(OrbisPadButtonDataOffset::TouchPad, pressed);
            break;
        case Style::Lunar:
        case Style::Jupiter:
            if (pressed) {
                TriggerButtonSwipe(controller,
                                   style == Style::Lunar ? BUTTON_SWIPE_UP : BUTTON_SWIPE_DOWN);
            }
            break;
        }
        return;
    }
    if (type == TypeB) {
        if (pressed) {
            const u32 taps = g_style.load() < 0 ? 1 : 2;
            auto cycle = std::make_unique<Cycle>(controller, ++g_cycle, style, taps, false);
            if (SDL_AddTimer(1, CycleStep, cycle.get()) != 0) {
                cycle.release();
            } else {
                LOG_ERROR(Input, "GR2port: no timer for a style change");
            }
        }
        return;
    }
    std::scoped_lock lock{g_chord_mutex};
    ++g_chord;
    if (pressed) {
        PressR2(controller, true);
        if (style != Style::Normal) {
            auto chord = std::make_unique<Chord>(controller, g_chord, type, style);
            if (SDL_AddTimer(ChordDelayMs, ChordStep, chord.get()) != 0) {
                chord.release();
            } else {
                LOG_ERROR(Input, "GR2port: no timer for a style change");
            }
        }
        return;
    }
    if (style != Style::Normal) {
        PressStyleInput(controller, type, style, false, left_y, right_y);
    }
    PressR2(controller, false);
}

} // namespace Input::GameFunctions
