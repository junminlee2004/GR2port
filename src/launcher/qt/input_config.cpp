// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <fstream>
#include <optional>
#include <set>

#include "input/game_functions.h"
#include "input/input_handler.h"
#include "launcher/qt/input_config.h"

namespace Gr2Port::Launcher {

namespace {

namespace Game = Input::GameFunctions;

constexpr std::string_view TouchSwipePrefix = "touchpad_swipe_enabled=";

// A config line without its comment and spaces, as the input config parser reads it.
std::string Compact(std::string_view line) {
    std::string compact(line.substr(0, line.find('#')));
    std::erase_if(compact, [](unsigned char c) { return std::isspace(c) != 0; });
    return compact;
}

std::string DeadzonePrefix(std::string_view stick) {
    return "analog_deadzone=" + std::string(stick) + ",";
}

} // namespace

Deadzones ReadDeadzones(const std::filesystem::path& file) {
    Deadzones deadzones;
    deadzones.fill({DeadzoneLow, DeadzoneHigh});
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        for (size_t i = 0; i < StickNames.size(); ++i) {
            const std::string prefix = DeadzonePrefix(StickNames[i]);
            if (!compact.starts_with(prefix)) {
                continue;
            }
            const char* const last = compact.data() + compact.size();
            int low = 0;
            int high = 0;
            const auto [comma, low_error] =
                std::from_chars(compact.data() + prefix.size(), last, low);
            if (low_error != std::errc{} || comma == last || *comma != ',') {
                continue;
            }
            const auto [end, high_error] = std::from_chars(comma + 1, last, high);
            if (high_error == std::errc{} && end == last) {
                deadzones[i] = {std::clamp(low, DeadzoneLow, DeadzoneHigh),
                                std::clamp(high, DeadzoneLow, DeadzoneHigh)};
            }
        }
    }
    return deadzones;
}

std::vector<ConfigLine> DeadzoneLines(const Deadzones& deadzones) {
    std::vector<ConfigLine> lines;
    for (size_t i = 0; i < StickNames.size(); ++i) {
        lines.push_back(
            {DeadzonePrefix(StickNames[i]), "analog_deadzone = " + std::string(StickNames[i]) +
                                                ", " + std::to_string(deadzones[i][0]) + ", " +
                                                std::to_string(deadzones[i][1])});
    }
    return lines;
}

ConfigLine TouchSwipeLine(bool on) {
    return {std::string(TouchSwipePrefix),
            std::string("touchpad_swipe_enabled = ") + (on ? "true" : "false")};
}

bool ReadTouchSwipe(const std::filesystem::path& file) {
    bool on = false;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        if (compact.starts_with(TouchSwipePrefix)) {
            const std::string_view value =
                std::string_view{compact}.substr(TouchSwipePrefix.size());
            on = value == "true" || value == "1";
        }
    }
    return on;
}

bool SaveConfigLines(const std::filesystem::path& file, const std::vector<ConfigLine>& changes) {
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            lines.push_back(std::move(line));
        }
    }
    std::vector<bool> written(changes.size());
    for (std::string& line : lines) {
        const std::string compact = Compact(line);
        for (size_t i = 0; i < changes.size(); ++i) {
            if (compact.starts_with(changes[i].prefix)) {
                line = changes[i].line;
                written[i] = true;
            }
        }
    }
    for (size_t i = 0; i < changes.size(); ++i) {
        if (!written[i]) {
            lines.push_back(changes[i].line);
        }
    }
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& line : lines) {
        out << line << '\n';
    }
    return !out.fail();
}

KeyBindings DefaultKeyBindings() {
    KeyBindings keys;
    for (const Game::Function& function : Game::Functions()) {
        keys.emplace_back(function.key);
    }
    return keys;
}

KeyBindings ReadKeyBindings(const std::filesystem::path& file) {
    const auto functions = Game::Functions();
    KeyBindings keys = DefaultKeyBindings();
    std::vector<bool> read(functions.size());
    bool any = false;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        const std::string compact = Compact(line);
        const auto equal = compact.find('=');
        const Game::Function* const function =
            equal == std::string::npos ? nullptr : Game::Find(compact.substr(0, equal));
        if (function == nullptr) {
            continue;
        }
        if (!any) {
            any = true;
            keys.assign(functions.size(), {});
        }
        const size_t index = function - functions.data();
        if (!read[index]) {
            read[index] = true;
            const std::string key = compact.substr(equal + 1);
            keys[index] = key == "unmapped" ? std::string{} : key;
        }
    }
    return keys;
}

bool SaveKeyBindings(const std::filesystem::path& file, const KeyBindings& keys) {
    const auto functions = Game::Functions();
    // The pad inputs the functions bind, and R2 and L3, which earlier configs gave keys.
    std::set<std::string_view> pad_outputs{"r2", "l3"};
    for (const Game::Function& function : functions) {
        if (!function.output.empty()) {
            pad_outputs.insert(function.output);
        }
    }
    const auto keyboard_only = [](std::string_view inputs) {
        while (!inputs.empty()) {
            const auto comma = inputs.find(',');
            if (!Input::string_to_keyboard_key_map.contains(std::string(inputs.substr(0, comma)))) {
                return false;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            inputs.remove_prefix(comma + 1);
        }
        return true;
    };
    const auto blank = [](std::string_view line) {
        return std::ranges::all_of(line, [](unsigned char c) { return std::isspace(c) != 0; });
    };
    // The removed lines leave no runs of blank lines.
    std::vector<std::string> lines;
    std::optional<size_t> block_at;
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            const std::string compact = Compact(line);
            const auto equal = compact.find('=');
            if (equal != std::string::npos) {
                const std::string output = compact.substr(0, equal);
                if (Game::Find(output) != nullptr ||
                    (pad_outputs.contains(output) && keyboard_only(compact.substr(equal + 1)))) {
                    block_at = block_at.value_or(lines.size());
                    continue;
                }
            }
            if (blank(line) && !lines.empty() && blank(lines.back())) {
                continue;
            }
            lines.push_back(std::move(line));
        }
    }
    std::vector<std::string> block;
    for (size_t i = 0; i < functions.size(); ++i) {
        const std::string& key = i < keys.size() ? keys[i] : std::string{};
        block.push_back(std::string(functions[i].name) + " = " + (key.empty() ? "unmapped" : key));
    }
    const size_t at = block_at.value_or(lines.size());
    if (at < lines.size() && !blank(lines[at])) {
        block.emplace_back();
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), block.begin(), block.end());
    std::ofstream out(file, std::ios::trunc);
    for (const std::string& line : lines) {
        out << line << '\n';
    }
    return !out.fail();
}

} // namespace Gr2Port::Launcher
