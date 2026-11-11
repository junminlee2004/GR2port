// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

#include "input/pad_bindings.h"

namespace Input::PadBindings {

namespace {

constexpr std::array<std::string_view, 16> Buttons{
    "cross", "circle", "square",  "triangle",        "l1",     "r1",       "l2",       "r2",
    "l3",    "r3",     "options", "touchpad_center", "pad_up", "pad_down", "pad_left", "pad_right",
};

// The pad buttons, triggers included, that a controller line can name as its input.
constexpr std::array<std::string_view, 16> PadInputs{
    "cross", "circle", "square",  "triangle", "l1",     "r1",       "l2",       "r2",
    "l3",    "r3",     "options", "back",     "pad_up", "pad_down", "pad_left", "pad_right",
};

std::string Compact(std::string_view line) {
    std::string compact{line.substr(0, line.find('#'))};
    std::erase_if(compact, [](unsigned char c) { return std::isspace(c) != 0; });
    return compact;
}

} // namespace

std::span<const std::string_view> GameButtons() {
    return Buttons;
}

bool IsPadInput(std::string_view name) {
    return std::ranges::find(PadInputs, name) != PadInputs.end();
}

Map Defaults() {
    Map buttons;
    for (const std::string_view button : Buttons) {
        buttons.emplace(button, button);
    }
    buttons["touchpad_center"] = "back";
    return buttons;
}

std::optional<std::pair<std::string, std::string>> ParseLine(std::string_view compact) {
    const auto equal = compact.find('=');
    if (equal == std::string_view::npos) {
        return std::nullopt;
    }
    std::string output{compact.substr(0, equal)};
    std::string input{compact.substr(equal + 1)};
    if (output == "touchpad" || output == "touchpad_left" || output == "touchpad_right") {
        output = "touchpad_center";
    }
    if (std::ranges::find(Buttons, output) == Buttons.end() ||
        (input != "unmapped" && !IsPadInput(input))) {
        return std::nullopt;
    }
    return std::pair{std::move(output), input == "unmapped" ? std::string{} : std::move(input)};
}

Map Read(const std::filesystem::path& file) {
    Map buttons;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (auto pad = ParseLine(Compact(line))) {
            const auto [at, added] = buttons.try_emplace(std::move(pad->first), pad->second);
            if (!added && at->second.empty()) {
                at->second = std::move(pad->second);
            }
        }
    }
    if (buttons.empty()) {
        return Defaults();
    }
    for (const std::string_view button : Buttons) {
        buttons.try_emplace(std::string(button));
    }
    return buttons;
}

} // namespace Input::PadBindings
