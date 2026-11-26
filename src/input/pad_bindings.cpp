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

constexpr std::array<std::string_view, 4> PaddleInputs{
    "lpaddle_high",
    "rpaddle_high",
    "lpaddle_low",
    "rpaddle_low",
};

// The photo functions and the button each presses with the hold input without a line.
constexpr std::array<std::string_view, 3> PhotoNames{"gr2_hide_ui_pad", "gr2_collection_pad",
                                                     "gr2_picture_roll_pad"};
constexpr std::array<std::string_view, 3> PhotoPresses{"r2", "l1", "r1"};

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

std::span<const std::string_view> Paddles() {
    return PaddleInputs;
}

bool IsPaddle(std::string_view name) {
    return std::ranges::find(PaddleInputs, name) != PaddleInputs.end();
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
        (input != "unmapped" && !IsPadInput(input) && !IsPaddle(input))) {
        return std::nullopt;
    }
    return std::pair{std::move(output), input == "unmapped" ? std::string{} : std::move(input)};
}

Map Read(const std::filesystem::path& file) {
    Map buttons;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (auto pad = ParseLine(Compact(line)); pad && !IsPaddle(pad->second)) {
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

Map ReadPaddles(const std::filesystem::path& file) {
    Map paddles;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (auto pad = ParseLine(Compact(line)); pad && IsPaddle(pad->second)) {
            paddles.try_emplace(std::move(pad->second), std::move(pad->first));
        }
    }
    return paddles;
}

std::span<const std::string_view> PhotoFunctions() {
    return PhotoNames;
}

bool IsPhotoBinding(std::string_view inputs) {
    const auto comma = inputs.find(',');
    if (comma == std::string_view::npos) {
        return IsPaddle(inputs);
    }
    const std::string_view hold = inputs.substr(0, comma);
    const std::string_view press = inputs.substr(comma + 1);
    return hold != press && (IsPadInput(hold) || IsPaddle(hold)) &&
           (IsPadInput(press) || IsPaddle(press));
}

Map PhotoDefaults(std::string_view hold) {
    Map bindings;
    for (size_t i = 0; i < PhotoNames.size(); ++i) {
        bindings.emplace(PhotoNames[i],
                         hold.empty() || hold == PhotoPresses[i]
                             ? std::string{}
                             : std::string(hold) + "," + std::string(PhotoPresses[i]));
    }
    return bindings;
}

bool IsPhotoLine(std::string_view compact) {
    const auto equal = compact.find('=');
    return equal != std::string_view::npos &&
           std::ranges::find(PhotoNames, compact.substr(0, equal)) != PhotoNames.end();
}

std::optional<std::pair<std::string, std::string>> ParsePhotoLine(std::string_view compact) {
    const auto equal = compact.find('=');
    if (equal == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view function = compact.substr(0, equal);
    const std::string_view inputs = compact.substr(equal + 1);
    if (std::ranges::find(PhotoNames, function) == PhotoNames.end() ||
        (inputs != "unmapped" && !IsPhotoBinding(inputs))) {
        return std::nullopt;
    }
    return std::pair{std::string(function),
                     inputs == "unmapped" ? std::string{} : std::string(inputs)};
}

Map ReadPhoto(const std::filesystem::path& file) {
    // The swipe combos' hold input: its last line that names one pad input.
    std::string hold = "l3";
    Map read;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        if (compact.starts_with("touchpad_swipe_combo_hold=")) {
            const std::string input = compact.substr(compact.find('=') + 1);
            if (input == "unmapped" || IsPadInput(input) || IsPaddle(input)) {
                hold = input == "unmapped" ? std::string{} : input;
            }
        } else if (auto photo = ParsePhotoLine(compact)) {
            // A function's last line counts, as in the game.
            read[std::move(photo->first)] = std::move(photo->second);
        }
    }
    Map bindings = PhotoDefaults(hold);
    for (auto& [function, binding] : read) {
        bindings[function] = std::move(binding);
    }
    return bindings;
}

} // namespace Input::PadBindings
