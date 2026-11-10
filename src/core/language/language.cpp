// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <cmrc/cmrc.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/language/language.h"

#ifdef _WIN32
#include <windows.h>
#endif

CMRC_DECLARE(gr2port_languages);

namespace Gr2Port::Language {

namespace {

constexpr int EnglishUnitedStates = 1;
constexpr int EnglishUnitedKingdom = 18;

// The PS4's order: Japanese, the Englishes, then the other languages with their regional forms
// beside each other.
constexpr std::array<int, 31> Order{0,  1,  18, 2,  22, 3,  20, 4,  5,  6,  7,  17, 8,  9,  10, 11,
                                    12, 13, 14, 15, 16, 19, 21, 23, 24, 25, 26, 27, 28, 29, 30};

std::mutex g_mutex;
// The language the table holds, or -1 before the first use.
int g_current = -1;
std::unordered_map<std::string, std::string> g_table;

std::optional<nlohmann::json> ReadFile(int console_language) {
    const cmrc::embedded_filesystem files = cmrc::gr2port_languages::get_filesystem();
    const std::string path = fmt::format("src/resources/languages/{}.json", console_language);
    if (!files.exists(path)) {
        return std::nullopt;
    }
    const cmrc::file file = files.open(path);
    nlohmann::json root = nlohmann::json::parse(file.begin(), file.end(), nullptr, false);
    if (!root.is_object()) {
        LOG_ERROR(Loader, "GR2port: the language table {} is not a JSON object", path);
        return std::nullopt;
    }
    return root;
}

// Adds a language's strings over those already in the table. A regional form names the language
// it differs from as its base, whose strings come first.
// Holds g_mutex.
void AddStrings(int console_language, int depth) {
    const std::optional<nlohmann::json> root = ReadFile(console_language);
    if (!root) {
        return;
    }
    if (const auto base = root->find("base");
        base != root->end() && base->is_number_integer() && depth < 2) {
        AddStrings(base->get<int>(), depth + 1);
    }
    const auto strings = root->find("strings");
    if (strings == root->end() || !strings->is_object()) {
        return;
    }
    for (const auto& [english, text] : strings->items()) {
        if (text.is_string()) {
            g_table.insert_or_assign(english, text.get<std::string>());
        }
    }
}

// Holds g_mutex.
void Load(int console_language) {
    g_current = console_language;
    g_table.clear();
    AddStrings(console_language, 0);
}

// Holds g_mutex.
const std::string* Find(std::string_view english) {
    if (g_current < 0) {
        Load(EmulatorSettings.GetConsoleLanguage());
    }
    const auto found = g_table.find(std::string{english});
    return found != g_table.end() ? &found->second : nullptr;
}

template <typename Char>
void ReplaceValues(std::basic_string<Char>& text, std::span<const std::basic_string<Char>> values) {
    for (size_t i = 0; i < values.size(); ++i) {
        const std::string marker = fmt::format("{{{}}}", i);
        const std::basic_string<Char> key(marker.begin(), marker.end());
        for (size_t at = text.find(key); at != std::basic_string<Char>::npos;
             at = text.find(key, at + values[i].size())) {
            text.replace(at, key.size(), values[i]);
        }
    }
}

#ifdef _WIN32
std::wstring Widen(std::string_view text) {
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

std::string Narrow(std::wstring_view text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                        length, nullptr, nullptr);
    return narrow;
}
#endif

} // namespace

std::span<const int> Listed() {
    return Order;
}

std::string Name(int console_language) {
    if (console_language == EnglishUnitedStates) {
        return "English (United States)";
    }
    if (console_language == EnglishUnitedKingdom) {
        return "English (United Kingdom)";
    }
    const std::optional<nlohmann::json> root = ReadFile(console_language);
    if (root && root->contains("name") && (*root)["name"].is_string()) {
        return (*root)["name"].get<std::string>();
    }
    return std::to_string(console_language);
}

int Current() {
    std::scoped_lock lock{g_mutex};
    if (g_current < 0) {
        Load(EmulatorSettings.GetConsoleLanguage());
    }
    return g_current;
}

void Use(int console_language) {
    std::scoped_lock lock{g_mutex};
    if (console_language != g_current) {
        Load(console_language);
    }
}

std::string Text(std::string_view english, std::span<const std::string> values) {
    std::string text;
    {
        std::scoped_lock lock{g_mutex};
        const std::string* found = Find(english);
        text = found != nullptr ? *found : std::string{english};
    }
    ReplaceValues(text, values);
    return text;
}

#ifdef _WIN32
std::wstring Text(std::wstring_view english, std::span<const std::wstring> values) {
    std::wstring text;
    {
        std::scoped_lock lock{g_mutex};
        const std::string* found = Find(Narrow(english));
        text = found != nullptr ? Widen(*found) : std::wstring{english};
    }
    ReplaceValues(text, values);
    return text;
}

std::wstring WideName(int console_language) {
    return Widen(Name(console_language));
}
#endif

} // namespace Gr2Port::Language
