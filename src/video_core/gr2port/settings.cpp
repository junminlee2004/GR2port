// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "video_core/gr2port/settings.h"

namespace Gr2Port::Settings {

namespace {

using json = nlohmann::json;

constexpr const char* Section = "GR2port";
// The prompt packs read the Halloween items mod pack's switch from here.
constexpr const char* ForkSection = "GR2Fork";
constexpr const char* HalloweenKey = "gr2HalloweenItems";

std::mutex g_mutex;
std::optional<Values> g_values;

std::filesystem::path ConfigPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "config.json";
}

json ReadConfig() {
    std::ifstream in{ConfigPath()};
    if (!in.good()) {
        return json::object();
    }
    json root = json::parse(in, nullptr, false);
    return root.is_object() ? root : json::object();
}

bool ReadBool(const json& section, const char* key, bool fallback) {
    const auto it = section.find(key);
    return it != section.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

u32 ReadU32(const json& section, const char* key, u32 fallback) {
    const auto it = section.find(key);
    return it != section.end() && it->is_number_unsigned() ? it->get<u32>() : fallback;
}

constexpr std::pair<VSync, std::string_view> VSyncNames[] = {
    {VSync::Off, "off"},
    {VSync::DoubleBuffered, "double"},
    {VSync::TripleBuffered, "triple"},
};

void ReadGame(const json& section, Game& game) {
    game.frame_rate = ReadU32(section, "frame_rate", game.frame_rate);
    if (const auto aspect = section.find("aspect");
        aspect != section.end() && aspect->is_string()) {
        game.aspect = aspect->get<std::string>();
    }
    if (const auto size = section.find("render_size"); size != section.end() && size->is_string()) {
        game.render_size = size->get<std::string>();
    }
    game.dlc_entitlements = ReadBool(section, "dlc_entitlements", game.dlc_entitlements);
    if (const auto extras = section.find("extras");
        extras != section.end() && extras->is_object()) {
        for (const auto& [name, on] : extras->items()) {
            if (on.is_boolean()) {
                game.extras.emplace(name, on.get<bool>());
            }
        }
    }
}

Values FromJson(const json& root) {
    Values values;
    if (const auto fork = root.find(ForkSection); fork != root.end() && fork->is_object()) {
        values.game.halloween_items = ReadBool(*fork, HalloweenKey, values.game.halloween_items);
    }
    const auto it = root.find(Section);
    if (it == root.end() || !it->is_object()) {
        return values;
    }
    const json& section = *it;
    ReadGame(section, values.game);
    Mouse& mouse = values.mouse;
    mouse.foot_horizontal = ReadU32(section, "mouse_foot_horizontal", mouse.foot_horizontal);
    mouse.foot_vertical = ReadU32(section, "mouse_foot_vertical", mouse.foot_vertical);
    mouse.shift_horizontal = ReadU32(section, "mouse_shift_horizontal", mouse.shift_horizontal);
    mouse.shift_vertical = ReadU32(section, "mouse_shift_vertical", mouse.shift_vertical);
    Display& display = values.display;
    display.fullscreen = ReadBool(section, "fullscreen", display.fullscreen);
    display.exclusive = ReadBool(section, "exclusive_fullscreen", display.exclusive);
    if (const auto monitor = section.find("monitor");
        monitor != section.end() && monitor->is_array() && monitor->size() == 2 &&
        (*monitor)[0].is_number_integer() && (*monitor)[1].is_number_integer()) {
        display.monitor = Point{(*monitor)[0].get<s32>(), (*monitor)[1].get<s32>()};
    }
    display.width = ReadU32(section, "width", display.width);
    display.height = ReadU32(section, "height", display.height);
    display.refresh_rate = ReadU32(section, "refresh_rate", display.refresh_rate);
    if (const auto vsync = section.find("vsync"); vsync != section.end() && vsync->is_string()) {
        for (const auto& [value, name] : VSyncNames) {
            if (vsync->get<std::string>() == name) {
                display.vsync = value;
            }
        }
    }
    return values;
}

json ToJson(const Values& values) {
    const Display& display = values.display;
    std::string_view vsync = "triple";
    for (const auto& [value, name] : VSyncNames) {
        if (display.vsync == value) {
            vsync = name;
        }
    }
    const Game& game = values.game;
    json extras = json::object();
    for (const auto& [name, on] : game.extras) {
        extras[name] = on;
    }
    return json{
        {"fullscreen", display.fullscreen},
        {"exclusive_fullscreen", display.exclusive},
        {"monitor",
         display.monitor ? json::array({display.monitor->x, display.monitor->y}) : json(nullptr)},
        {"width", display.width},
        {"height", display.height},
        {"refresh_rate", display.refresh_rate},
        {"vsync", std::string(vsync)},
        {"frame_rate", game.frame_rate},
        {"aspect", game.aspect},
        {"render_size", game.render_size},
        {"dlc_entitlements", game.dlc_entitlements},
        {"extras", std::move(extras)},
        {"mouse_foot_horizontal", values.mouse.foot_horizontal},
        {"mouse_foot_vertical", values.mouse.foot_vertical},
        {"mouse_shift_horizontal", values.mouse.shift_horizontal},
        {"mouse_shift_vertical", values.mouse.shift_vertical},
    };
}

const Values& Loaded() {
    if (!g_values) {
        g_values = FromJson(ReadConfig());
    }
    return *g_values;
}

bool Write(const Values& values) {
    g_values = values;
    json root = ReadConfig();
    root[Section] = ToJson(values);
    if (!root.contains(ForkSection) || !root[ForkSection].is_object()) {
        root[ForkSection] = json::object();
    }
    root[ForkSection][HalloweenKey] = values.game.halloween_items;
    std::ofstream out{ConfigPath(), std::ios::trunc};
    out << root.dump(4);
    if (!out.good()) {
        LOG_ERROR(Config, "GR2port: could not write {}",
                  Common::FS::PathToUTF8String(ConfigPath()));
        return false;
    }
    return true;
}

} // namespace

Values Get() {
    std::scoped_lock lock{g_mutex};
    return Loaded();
}

bool Set(const Values& values) {
    std::scoped_lock lock{g_mutex};
    return Write(values);
}

} // namespace Gr2Port::Settings
