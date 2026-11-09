// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <string_view>

#include "common/elf_info.h"
#include "core/emulator_settings.h"
#include "core/prompt_packs/keyboard_prompts.h"
#include "input/input_handler.h"

namespace PromptPacks::Keyboard {

namespace {

// ------------------------------------------------------------------------------------- keys
struct NamedKey {
    std::string_view name;
    std::string_view text;
    std::string_view symbol;
};

// The input config's key names that are not a letter, a digit or F1-F12.
constexpr NamedKey NamedKeys[]{
    {"grave", "`", ""},
    {"tilde", "~", ""},
    {"exclamation", "!", ""},
    {"at", "@", ""},
    {"hash", "#", ""},
    {"dollar", "$", ""},
    {"percent", "%", ""},
    {"caret", "^", ""},
    {"ampersand", "&", ""},
    {"asterisk", "*", ""},
    {"lparen", "(", ""},
    {"rparen", ")", ""},
    {"minus", "-", ""},
    {"underscore", "_", ""},
    {"equals", "=", ""},
    {"plus", "+", ""},
    {"lbracket", "[", ""},
    {"rbracket", "]", ""},
    {"lbrace", "{", ""},
    {"rbrace", "}", ""},
    {"backslash", "\\", ""},
    {"pipe", "|", ""},
    {"semicolon", ";", ""},
    {"colon", ":", ""},
    {"apostrophe", "'", ""},
    {"quote", "\"", ""},
    {"comma", ",", ""},
    {"less", "<", ""},
    {"period", ".", ""},
    {"greater", ">", ""},
    {"slash", "/", ""},
    {"question", "?", ""},
    {"escape", "ESC", ""},
    {"printscreen", "PRTSC", ""},
    {"scrolllock", "SCRLK", ""},
    {"pausebreak", "PAUSE", ""},
    {"backspace", "BKSP", ""},
    {"delete", "DEL", ""},
    {"insert", "INS", ""},
    {"home", "HOME", ""},
    {"end", "END", ""},
    {"pgup", "PGUP", ""},
    {"pgdown", "PGDN", ""},
    {"tab", "TAB", ""},
    {"capslock", "CAPS", ""},
    {"enter", "ENTER", ""},
    {"lshift", "", "shift"},
    {"rshift", "", "shift"},
    {"lctrl", "CTRL", ""},
    {"rctrl", "CTRL", ""},
    {"lalt", "ALT", ""},
    {"ralt", "ALT", ""},
    {"lmeta", "WIN", ""},
    {"rmeta", "WIN", ""},
    {"lwin", "WIN", ""},
    {"rwin", "WIN", ""},
    {"space", "", "space"},
    {"up", "", "arrow_up"},
    {"down", "", "arrow_down"},
    {"left", "", "arrow_left"},
    {"right", "", "arrow_right"},
    {"kp0", "NUM0", ""},
    {"kp1", "NUM1", ""},
    {"kp2", "NUM2", ""},
    {"kp3", "NUM3", ""},
    {"kp4", "NUM4", ""},
    {"kp5", "NUM5", ""},
    {"kp6", "NUM6", ""},
    {"kp7", "NUM7", ""},
    {"kp8", "NUM8", ""},
    {"kp9", "NUM9", ""},
    {"kpperiod", "NUM.", ""},
    {"kpcomma", "NUM,", ""},
    {"kpslash", "NUM/", ""},
    {"kpasterisk", "NUM*", ""},
    {"kpminus", "NUM-", ""},
    {"kpplus", "NUM+", ""},
    {"kpequals", "NUM=", ""},
    {"kpenter", "NUMENT", ""},
    {"leftbutton", "", "lmb"},
    {"rightbutton", "", "rmb"},
    {"middlebutton", "", "mmb"},
    {"sidebuttonback", "", "mouse_back"},
    {"sidebuttonforward", "", "mouse_forward"},
    {"mousewheelup", "", "wheel_up"},
    {"mousewheeldown", "", "wheel_down"},
    {"mousewheelleft", "", "wheel_left"},
    {"mousewheelright", "", "wheel_right"},
};

// The prompt of a key name of the input config; none for gamepad inputs and unknown names.
std::optional<Label> KeyLabel(std::string_view name) {
    if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) {
        return Label{std::string(1, static_cast<char>(std::toupper(name[0]))), {}};
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f' &&
        std::all_of(name.begin() + 1, name.end(), [](char c) { return std::isdigit(c); })) {
        const int number = std::stoi(std::string(name.substr(1)));
        if (number >= 1 && number <= 12) {
            return Label{"F" + std::to_string(number), {}};
        }
    }
    for (const NamedKey& key : NamedKeys) {
        if (key.name == name) {
            return Label{std::string(key.text), std::string(key.symbol)};
        }
    }
    return std::nullopt;
}

// Config outputs that slots name, and the slot button each one labels.
constexpr std::array<std::pair<std::string_view, std::string_view>, 17> Outputs{{
    {"cross", "cross"},
    {"circle", "circle"},
    {"square", "square"},
    {"triangle", "triangle"},
    {"l1", "L1"},
    {"r1", "R1"},
    {"l2", "L2"},
    {"r2", "R2"},
    {"l3", "L3"},
    {"r3", "R3"},
    {"options", "OPTIONS"},
    {"touchpad_center", "touchpad"},
    {"touchpad_left", "touchpad"},
    {"touchpad_right", "touchpad"},
    {"touchpad", "touchpad"},
    {"touchpad_swipe_up", "swipe_up"},
    {"touchpad_swipe_down", "swipe_down"},
}};

// The left stick's four directions, in the order the move label lists them: up, left, down, right.
constexpr std::array<std::string_view, 4> MoveOutputs{"axis_left_y_minus", "axis_left_x_minus",
                                                      "axis_left_y_plus", "axis_left_x_plus"};

const Label Unbound{{}, "minus"};

// The keys of the move label: WASD when each is one character, the arrow cluster for the arrow
// keys, a dash when a direction has no key.
Label MoveLabel(const std::array<std::optional<Label>, 4>& keys) {
    if (std::any_of(keys.begin(), keys.end(), [](const auto& key) { return !key; })) {
        return Unbound;
    }
    constexpr std::array<std::string_view, 4> Arrows{"arrow_up", "arrow_left", "arrow_down",
                                                     "arrow_right"};
    bool arrows = true;
    bool letters = true;
    std::string text;
    for (u32 i = 0; i < 4; ++i) {
        arrows = arrows && keys[i]->symbol == Arrows[i];
        letters = letters && keys[i]->text.size() == 1;
        text += keys[i]->text;
    }
    if (arrows) {
        return Label{{}, "arrows"};
    }
    return letters ? Label{text, {}} : Label{keys[0]->text, keys[0]->symbol};
}

} // namespace

std::map<std::string, Label> BoundLabels() {
    const std::string serial{Common::ElfInfo::Instance().GameSerial()};
    const auto path =
        Input::GetInputConfigFile(EmulatorSettings.IsUseUnifiedInputConfig() ? "default" : serial);
    std::map<std::string, Label> labels;
    std::map<std::string, Label> outputs;
    std::ifstream in{path};
    for (std::string line; std::getline(in, line);) {
        line = line.substr(0, line.find('#'));
        std::erase_if(line, [](unsigned char c) { return std::isspace(c) != 0; });
        const auto equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string output = line.substr(0, equal);
        if (outputs.contains(output)) {
            continue;
        }
        // The last key of a combination is the one the prompt shows.
        std::optional<Label> label;
        std::string_view inputs{line};
        inputs.remove_prefix(equal + 1);
        while (!inputs.empty()) {
            const auto comma = inputs.find(',');
            const std::string_view name = inputs.substr(0, comma);
            if (auto key = KeyLabel(name)) {
                label = std::move(key);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            inputs.remove_prefix(comma + 1);
        }
        if (label) {
            outputs.emplace(output, std::move(*label));
        }
    }
    for (const auto& [output, button] : Outputs) {
        const std::string name{button};
        if (const auto it = outputs.find(std::string(output));
            it != outputs.end() && !labels.contains(name)) {
            labels.emplace(name, it->second);
        }
    }
    std::array<std::optional<Label>, 4> move{};
    for (u32 i = 0; i < 4; ++i) {
        if (const auto it = outputs.find(std::string(MoveOutputs[i])); it != outputs.end()) {
            move[i] = it->second;
        }
    }
    labels["Lstick"] = MoveLabel(move);
    // The mouse always turns the camera.
    labels["Rstick"] = Label{{}, "mouse"};
    return labels;
}

} // namespace PromptPacks::Keyboard
