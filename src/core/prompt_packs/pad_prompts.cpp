// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <optional>

#include "core/prompt_packs/pad_prompts.h"

namespace PromptPacks::Pad {

namespace {

using Keyboard::Label;

// The slot buttons that show a game button, and that game button by the input config's name.
constexpr std::array<std::pair<std::string_view, std::string_view>, 12> GameSlots{{
    {"cross", "cross"},
    {"circle", "circle"},
    {"square", "square"},
    {"triangle", "triangle"},
    {"L1", "l1"},
    {"R1", "r1"},
    {"L2", "l2"},
    {"R2", "r2"},
    {"L3", "l3"},
    {"R3", "r3"},
    {"OPTIONS", "options"},
    {"touchpad", "touchpad_center"},
}};

// A pad button by the input config's name, as the slot names call it: "l1" is "L1", "back" and the
// d-pad keep their names.
std::string_view SlotName(std::string_view pad_button) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 7> Renamed{{
        {"l1", "L1"},
        {"r1", "R1"},
        {"l2", "L2"},
        {"r2", "R2"},
        {"l3", "L3"},
        {"r3", "R3"},
        {"options", "OPTIONS"},
    }};
    for (const auto& [from, to] : Renamed) {
        if (from == pad_button) {
            return to;
        }
    }
    return pad_button;
}

// The Xbox face letters' colours, as the Xbox pack draws them.
constexpr std::array<u8, 3> XboxA{102, 209, 109};
constexpr std::array<u8, 3> XboxB{240, 90, 81};
constexpr std::array<u8, 3> XboxX{83, 169, 250};
constexpr std::array<u8, 3> XboxY{246, 214, 74};

// The PlayStation face marks' colours in the game's own prompts.
constexpr std::array<u8, 3> PsCross{82, 161, 251};
constexpr std::array<u8, 3> PsCircle{255, 64, 0};
constexpr std::array<u8, 3> PsSquare{255, 95, 255};
constexpr std::array<u8, 3> PsTriangle{53, 230, 154};

Label Text(std::string text, std::optional<std::array<u8, 3>> rgb = std::nullopt) {
    return Label{std::move(text), {}, rgb};
}

Label Mark(std::string symbol, std::optional<std::array<u8, 3>> rgb = std::nullopt) {
    return Label{{}, std::move(symbol), rgb};
}

// A button's label on the layout's pad, by slot name; d-pad directions and touch pad flicks show
// arrows on every pad.
std::optional<Label> PadLabel(std::string_view layout, std::string_view button) {
    if (button == "pad_up" || button == "swipe_up") {
        return Mark("arrow_up");
    }
    if (button == "pad_down" || button == "swipe_down") {
        return Mark("arrow_down");
    }
    if (button == "pad_left") {
        return Mark("arrow_left");
    }
    if (button == "pad_right") {
        return Mark("arrow_right");
    }
    const bool steam = layout == "steamdeck" || layout == "steamcontroller";
    struct Row {
        std::string_view button;
        Label playstation;
        Label xbox;
        Label nintendo;
        Label valve;
    };
    // Valve's pads: Xbox face letters in white, Valve's shoulder names; Back is View.
    const std::array<Row, 16> Rows{{
        {"cross", Mark("ps_cross", PsCross), Text("A", XboxA), Text("B"), Text("A")},
        {"circle", Mark("ps_circle", PsCircle), Text("B", XboxB), Text("A"), Text("B")},
        {"square", Mark("ps_square", PsSquare), Text("X", XboxX), Text("Y"), Text("X")},
        {"triangle", Mark("ps_triangle", PsTriangle), Text("Y", XboxY), Text("X"), Text("Y")},
        {"L1", Text("L1"), Text("LB"), Text("L"), Text("L1")},
        {"R1", Text("R1"), Text("RB"), Text("R"), Text("R1")},
        {"L2", Text("L2"), Text("LT"), Text("ZL"), Text("L2")},
        {"R2", Text("R2"), Text("RT"), Text("ZR"), Text("R2")},
        {"L3", Text("L3"), Text("LS"), Text("L"), Text("L")},
        {"R3", Text("R3"), Text("RS"), Text("R"), Text("R")},
        {"Lstick", Text("L"), Text("LS"), Text("L"), Text("L")},
        {"Rstick", Text("R"), Text("RS"), Text("R"), Text("R")},
        {"OPTIONS", Text("OPTIONS"), Mark("menu"), Mark("plus"), Mark("menu")},
        {"SHARE", Text("SHARE"), Mark("share"), Mark("capture"), Mark("qam")},
        {"touchpad", Mark("touchpad"), Mark("view"), Mark("minus"), Mark("view")},
        {"back", Text("SHARE"), Mark("view"), Mark("minus"), Mark("view")},
    }};
    const auto row = std::ranges::find(Rows, button, &Row::button);
    if (row == Rows.end()) {
        return std::nullopt;
    }
    if (layout == "xbox") {
        return row->xbox;
    }
    if (layout == "switch") {
        return row->nintendo;
    }
    return steam ? row->valve : row->playstation;
}

} // namespace

std::set<std::string> Changed(const Input::PadBindings::Map& buttons) {
    const Input::PadBindings::Map defaults = Input::PadBindings::Defaults();
    std::set<std::string> changed;
    for (const auto& [slot, game_button] : GameSlots) {
        const auto bound = buttons.find(game_button);
        const auto fallback = defaults.find(game_button);
        if (bound == buttons.end() || fallback == defaults.end() ||
            bound->second != fallback->second) {
            changed.emplace(slot);
        }
    }
    return changed;
}

std::map<std::string, Label> Labels(std::string_view layout,
                                    const Input::PadBindings::Map& buttons) {
    std::map<std::string, Label> labels;
    for (const std::string_view own : {"Lstick", "Rstick", "SHARE", "swipe_up", "swipe_down"}) {
        if (auto label = PadLabel(layout, own)) {
            labels.emplace(own, std::move(*label));
        }
    }
    for (const auto& [slot, game_button] : GameSlots) {
        const auto bound = buttons.find(game_button);
        if (bound == buttons.end() || bound->second.empty()) {
            continue;
        }
        if (auto label = PadLabel(layout, SlotName(bound->second))) {
            labels.emplace(slot, std::move(*label));
        }
    }
    return labels;
}

} // namespace PromptPacks::Pad
