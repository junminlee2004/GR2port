// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "graphics/gr2port/settings.h"

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

constexpr std::pair<Quality, std::string_view> QualityNames[] = {
    {Quality::Original, "original"},
    {Quality::Medium, "medium"},
    {Quality::Low, "low"},
    {Quality::Off, "off"},
};

// The shadows also take Max.
constexpr std::pair<Quality, std::string_view> ShadowQualityNames[] = {
    {Quality::Original, "original"}, {Quality::Medium, "medium"}, {Quality::Low, "low"},
    {Quality::Off, "off"},           {Quality::Max, "max"},
};

constexpr std::pair<Upscaler, std::string_view> UpscalerNames[] = {
    {Upscaler::Off, "off"},   {Upscaler::Fsr3, "fsr3"}, {Upscaler::Fsr4, "fsr4"},
    {Upscaler::Dlss, "dlss"}, {Upscaler::Xess, "xess"},
};

constexpr std::pair<LowLatency, std::string_view> LowLatencyNames[] = {
    {LowLatency::Off, "off"},
    {LowLatency::Reflex, "reflex"},
    {LowLatency::AntiLag2, "anti_lag_2"},
    {LowLatency::Universal, "universal"},
};

constexpr std::pair<UpscaleQuality, std::string_view> UpscaleQualityNames[] = {
    {UpscaleQuality::Native, "native"},
    {UpscaleQuality::Quality, "quality"},
    {UpscaleQuality::Balanced, "balanced"},
    {UpscaleQuality::Performance, "performance"},
    {UpscaleQuality::UltraPerformance, "ultra_performance"},
};

template <typename Value, std::size_t Count>
Value ReadName(const json& section, const char* key, Value fallback,
               const std::pair<Value, std::string_view> (&names)[Count]) {
    const auto it = section.find(key);
    if (it == section.end() || !it->is_string()) {
        return fallback;
    }
    for (const auto& [value, name] : names) {
        if (it->get<std::string>() == name) {
            return value;
        }
    }
    return fallback;
}

template <typename Value, std::size_t Count>
std::string NameOf(Value value, const std::pair<Value, std::string_view> (&names)[Count]) {
    for (const auto& [entry, name] : names) {
        if (entry == value) {
            return std::string(name);
        }
    }
    return std::string(names[0].second);
}

// A quality that older files hold as a switch: true is Original, false Off.
Quality ReadSwitchedQuality(const json& section, const char* key, Quality fallback) {
    const auto it = section.find(key);
    if (it != section.end() && it->is_boolean()) {
        return it->get<bool>() ? Quality::Original : Quality::Off;
    }
    return ReadName(section, key, fallback, QualityNames);
}

void ReadGraphics(const json& section, Graphics& graphics) {
    if (const auto preset = section.find("graphics_preset");
        preset != section.end() && preset->is_string()) {
        graphics.preset = preset->get<std::string>();
    }
    graphics.shadows = ReadName(section, "shadow_quality", graphics.shadows, ShadowQualityNames);
    graphics.fog = ReadName(section, "fog_quality", graphics.fog, QualityNames);
    if (graphics.fog == Quality::Off) {
        graphics.fog = Quality::Low;
    }
    graphics.ambient_occlusion =
        ReadSwitchedQuality(section, "ambient_occlusion", graphics.ambient_occlusion);
    graphics.motion_blur = ReadSwitchedQuality(section, "motion_blur", graphics.motion_blur);
    graphics.antialiasing = ReadBool(section, "antialiasing", graphics.antialiasing);
    graphics.bloom = ReadBool(section, "bloom", graphics.bloom);
    graphics.outlines = ReadBool(section, "outlines", graphics.outlines);
    graphics.decals = ReadBool(section, "decals", graphics.decals);
    graphics.glass_refraction = ReadBool(section, "glass_refraction", graphics.glass_refraction);
    graphics.fog_shadows = ReadBool(section, "fog_shadows", graphics.fog_shadows);
    graphics.half_resolution_particles =
        ReadBool(section, "half_resolution_particles", graphics.half_resolution_particles);
    graphics.smoke_and_dust = ReadBool(section, "smoke_and_dust", graphics.smoke_and_dust);
    graphics.distortion = ReadBool(section, "distortion", graphics.distortion);
    const u32 anisotropy =
        ReadU32(section, "anisotropic_filtering", graphics.anisotropic_filtering);
    if (anisotropy == 0 || anisotropy == 2 || anisotropy == 4 || anisotropy == 8 ||
        anisotropy == 16) {
        graphics.anisotropic_filtering = anisotropy;
    }
    const auto detail = [&](const char* key, std::span<const u32> levels, u32& percent) {
        const u32 value = ReadU32(section, key, percent);
        if (std::ranges::find(levels, value) != levels.end()) {
            percent = value;
        }
    };
    detail("far_environment_detail", FarDetailLevels, graphics.far_environment_detail);
    // Older files hold the near distances as world_detail.
    detail("world_detail", DetailLevels, graphics.near_environment_detail);
    detail("near_environment_detail", DetailLevels, graphics.near_environment_detail);
    detail("character_detail", DetailLevels, graphics.character_detail);
    detail("shadow_distance", ShadowDistanceLevels, graphics.shadow_distance);
    detail("field_of_view", FieldOfViewLevels, graphics.field_of_view);
    detail("screen_shake", ScreenShakeLevels, graphics.screen_shake);
    detail("upscale_sharpening", UpscaleSharpeningLevels, graphics.upscale_sharpening);
    graphics.hud_gravity_meter = ReadBool(section, "hud_gravity_meter", graphics.hud_gravity_meter);
    graphics.hud_health_bar = ReadBool(section, "hud_health_bar", graphics.hud_health_bar);
    graphics.hud_crosshair = ReadBool(section, "hud_crosshair", graphics.hud_crosshair);
    graphics.upscaler = ReadName(section, "upscaler", graphics.upscaler, UpscalerNames);
    graphics.upscale_quality =
        ReadName(section, "upscale_quality", graphics.upscale_quality, UpscaleQualityNames);
    graphics.frame_generation = ReadBool(section, "frame_generation", graphics.frame_generation);
    graphics.low_latency = ReadName(section, "low_latency", graphics.low_latency, LowLatencyNames);
    graphics.texture_streaming_mb =
        ReadU32(section, "texture_streaming_mb", graphics.texture_streaming_mb);
    graphics.pass_times = ReadBool(section, "pass_times", graphics.pass_times);
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
    game.screenshot_popup = ReadBool(section, "screenshot_popup", game.screenshot_popup);
    game.low_energy_alert =
        std::min<u32>(ReadU32(section, "low_energy_alert_volume", game.low_energy_alert), 100);
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
    ReadGraphics(section, values.graphics);
    Mouse& mouse = values.mouse;
    mouse.foot_horizontal = ReadU32(section, "mouse_foot_horizontal", mouse.foot_horizontal);
    mouse.foot_vertical = ReadU32(section, "mouse_foot_vertical", mouse.foot_vertical);
    mouse.shift_horizontal = ReadU32(section, "mouse_shift_horizontal", mouse.shift_horizontal);
    mouse.shift_vertical = ReadU32(section, "mouse_shift_vertical", mouse.shift_vertical);
    if (const auto prompts = section.find("prompts");
        prompts != section.end() && prompts->is_string()) {
        mouse.prompts = prompts->get<std::string>();
    }
    mouse.control_type = ReadU32(section, "control_type", mouse.control_type);
    mouse.special_attack_camera = std::min<u32>(
        ReadU32(section, "special_attack_camera_sensitivity", mouse.special_attack_camera), 200);
    mouse.panther_camera =
        std::min<u32>(ReadU32(section, "panther_camera_sensitivity", mouse.panther_camera), 200);
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
    display.hdr = ReadBool(section, "hdr", display.hdr);
    display.hdr_peak_nits = ReadU32(section, "hdr_peak_nits", display.hdr_peak_nits);
    display.hdr_paper_white_nits =
        ReadU32(section, "hdr_paper_white_nits", display.hdr_paper_white_nits);
    display.gpu_crash_catcher = ReadBool(section, "gpu_crash_catcher", display.gpu_crash_catcher);
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
        {"hdr", display.hdr},
        {"hdr_peak_nits", display.hdr_peak_nits},
        {"hdr_paper_white_nits", display.hdr_paper_white_nits},
        {"gpu_crash_catcher", display.gpu_crash_catcher},
        {"frame_rate", game.frame_rate},
        {"aspect", game.aspect},
        {"render_size", game.render_size},
        {"dlc_entitlements", game.dlc_entitlements},
        {"screenshot_popup", game.screenshot_popup},
        {"low_energy_alert_volume", game.low_energy_alert},
        {"extras", std::move(extras)},
        {"mouse_foot_horizontal", values.mouse.foot_horizontal},
        {"mouse_foot_vertical", values.mouse.foot_vertical},
        {"mouse_shift_horizontal", values.mouse.shift_horizontal},
        {"mouse_shift_vertical", values.mouse.shift_vertical},
        {"prompts", values.mouse.prompts},
        {"control_type", values.mouse.control_type},
        {"special_attack_camera_sensitivity", values.mouse.special_attack_camera},
        {"panther_camera_sensitivity", values.mouse.panther_camera},
        {"graphics_preset", values.graphics.preset},
        {"shadow_quality", NameOf(values.graphics.shadows, ShadowQualityNames)},
        {"fog_quality", NameOf(values.graphics.fog, QualityNames)},
        {"ambient_occlusion", NameOf(values.graphics.ambient_occlusion, QualityNames)},
        {"motion_blur", NameOf(values.graphics.motion_blur, QualityNames)},
        {"antialiasing", values.graphics.antialiasing},
        {"bloom", values.graphics.bloom},
        {"outlines", values.graphics.outlines},
        {"decals", values.graphics.decals},
        {"glass_refraction", values.graphics.glass_refraction},
        {"fog_shadows", values.graphics.fog_shadows},
        {"half_resolution_particles", values.graphics.half_resolution_particles},
        {"smoke_and_dust", values.graphics.smoke_and_dust},
        {"distortion", values.graphics.distortion},
        {"anisotropic_filtering", values.graphics.anisotropic_filtering},
        {"far_environment_detail", values.graphics.far_environment_detail},
        {"near_environment_detail", values.graphics.near_environment_detail},
        {"character_detail", values.graphics.character_detail},
        {"shadow_distance", values.graphics.shadow_distance},
        {"field_of_view", values.graphics.field_of_view},
        {"screen_shake", values.graphics.screen_shake},
        {"upscaler", NameOf(values.graphics.upscaler, UpscalerNames)},
        {"upscale_quality", NameOf(values.graphics.upscale_quality, UpscaleQualityNames)},
        {"upscale_sharpening", values.graphics.upscale_sharpening},
        {"frame_generation", values.graphics.frame_generation},
        {"low_latency", NameOf(values.graphics.low_latency, LowLatencyNames)},
        {"hud_gravity_meter", values.graphics.hud_gravity_meter},
        {"hud_health_bar", values.graphics.hud_health_bar},
        {"hud_crosshair", values.graphics.hud_crosshair},
        {"texture_streaming_mb", values.graphics.texture_streaming_mb},
        {"pass_times", values.graphics.pass_times},
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
    // The file is written beside config.json and then takes its name, so a quit while it is
    // written leaves the old file whole.
    std::filesystem::path written = ConfigPath();
    written += ".new";
    {
        std::ofstream out{written, std::ios::trunc};
        out << root.dump(4);
        out.flush();
        if (!out.good()) {
            LOG_ERROR(Config, "GR2port: could not write {}", Common::FS::PathToUTF8String(written));
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(written, ConfigPath(), error);
    if (error) {
        LOG_ERROR(Config, "GR2port: could not replace {}: {}",
                  Common::FS::PathToUTF8String(ConfigPath()), error.message());
        return false;
    }
    return true;
}

} // namespace

Graphics FromPreset(const Preset& preset) {
    return Graphics{
        .preset = std::string(preset.key),
        .shadows = preset.shadows,
        .fog = preset.fog,
        .ambient_occlusion = preset.ambient_occlusion,
        .motion_blur = preset.motion_blur,
        .antialiasing = preset.antialiasing,
        .bloom = preset.bloom,
        .outlines = preset.outlines,
        .decals = preset.decals,
        .glass_refraction = preset.glass_refraction,
        .fog_shadows = preset.fog_shadows,
        .half_resolution_particles = preset.half_resolution_particles,
        .smoke_and_dust = preset.smoke_and_dust,
        .distortion = preset.distortion,
        .anisotropic_filtering = preset.anisotropic_filtering,
        .far_environment_detail = preset.far_environment_detail,
        .near_environment_detail = preset.near_environment_detail,
        .character_detail = preset.character_detail,
    };
}

std::string_view MatchPreset(const Game& game, const Graphics& graphics) {
    for (const Preset& preset : Presets) {
        const Graphics passes = FromPreset(preset);
        const bool same_passes =
            passes.shadows == graphics.shadows && passes.fog == graphics.fog &&
            passes.ambient_occlusion == graphics.ambient_occlusion &&
            passes.motion_blur == graphics.motion_blur &&
            passes.antialiasing == graphics.antialiasing && passes.bloom == graphics.bloom &&
            passes.outlines == graphics.outlines && passes.decals == graphics.decals &&
            passes.glass_refraction == graphics.glass_refraction &&
            passes.fog_shadows == graphics.fog_shadows &&
            passes.half_resolution_particles == graphics.half_resolution_particles &&
            passes.smoke_and_dust == graphics.smoke_and_dust &&
            passes.distortion == graphics.distortion &&
            passes.anisotropic_filtering == graphics.anisotropic_filtering &&
            passes.far_environment_detail == graphics.far_environment_detail &&
            passes.near_environment_detail == graphics.near_environment_detail &&
            passes.character_detail == graphics.character_detail;
        const bool same_size = preset.aspect.empty() || (preset.aspect == game.aspect &&
                                                         preset.render_size == game.render_size);
        const bool same_streaming = !preset.texture_streaming_mb ||
                                    *preset.texture_streaming_mb == graphics.texture_streaming_mb;
        const bool same_distance =
            !preset.shadow_distance || *preset.shadow_distance == graphics.shadow_distance;
        if (same_passes && same_size && same_streaming && same_distance) {
            return preset.key;
        }
    }
    return "custom";
}

Values Get() {
    std::scoped_lock lock{g_mutex};
    return Loaded();
}

bool Set(const Values& values) {
    std::scoped_lock lock{g_mutex};
    return Write(values);
}

std::string Describe() {
    std::scoped_lock lock{g_mutex};
    json values = ToJson(Loaded());
    values[HalloweenKey] = Loaded().game.halloween_items;
    return values.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool Update(const std::function<void(Values&)>& change) {
    std::scoped_lock lock{g_mutex};
    Values values = Loaded();
    change(values);
    return Write(values);
}

} // namespace Gr2Port::Settings
