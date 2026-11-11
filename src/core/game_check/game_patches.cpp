// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <charconv>
#include <string>
#include <pugixml.hpp>

#include "core/emulator_settings.h"
#include "core/game_check/game_check.h"
#include "core/game_check/game_patches.h"
#include "graphics/gr2port/settings.h"

namespace Gr2Port::GamePatches {

namespace {

// The frame rate patches of every version, which the frame rate setting alone turns on.
bool IsFrameRatePatch(std::string_view name) {
    return name.find("fps v5.0") != std::string_view::npos ||
           name.find("fps v6.0") != std::string_view::npos;
}

bool IsResolutionPatch(std::string_view category) {
    return category.starts_with("Resolution/");
}

std::string_view FrameRatePatch(u32 fps) {
    const auto rate = std::ranges::find(FrameRates, fps, &FrameRate::fps);
    return rate != FrameRates.end() ? rate->patch : std::string_view{};
}

const Aspect& ChosenAspect(const std::string& name) {
    const auto aspect = std::ranges::find(Aspects, std::string_view{name}, &Aspect::name);
    return aspect != Aspects.end() ? *aspect : Aspects.front();
}

// Whether a resolution patch's name holds the chosen size's "x<height>".
bool IsChosenSize(std::string_view name, const Aspect& aspect, const std::string& size) {
    const auto chosen = std::ranges::find(RenderSizes, std::string_view{size}, &RenderSize::name);
    const u32 lines = chosen != RenderSizes.end() ? chosen->lines : 1080;
    const u32 height = aspect.ratio < 16.0f / 9.0f ? lines * 10 / 9 : lines;
    const std::string tag = "x" + std::to_string(height);
    for (size_t at = name.find(tag); at != std::string_view::npos; at = name.find(tag, at + 1)) {
        const size_t end = at + tag.size();
        if (end == name.size() || name[end] < '0' || name[end] > '9') {
            return true;
        }
    }
    return false;
}

// The extra direct memory, in MB, that the chosen resolution patch's note asks for.
int ResolutionDmem() {
    constexpr std::string_view Needs = "Additional DMem Allocation ";
    pugi::xml_document doc;
    const std::string_view xml = GameCheck::PatchXml();
    if (!doc.load_buffer(xml.data(), xml.size())) {
        return 0;
    }
    for (const pugi::xml_node patch : doc.child("Patch").children("Metadata")) {
        const std::string_view category = patch.attribute("Category").value();
        if (!IsResolutionPatch(category) ||
            !IsEnabled(patch.attribute("Name").value(), category, false)) {
            continue;
        }
        const std::string_view note = patch.attribute("Note").value();
        const size_t at = note.find(Needs);
        int dmem = 0;
        if (at != std::string_view::npos) {
            const std::string_view digits = note.substr(at + Needs.size());
            std::from_chars(digits.data(), digits.data() + digits.size(), dmem);
        }
        return dmem;
    }
    return 0;
}

} // namespace

std::vector<Extra> Extras() {
    std::vector<Extra> extras;
    pugi::xml_document doc;
    const std::string_view xml = GameCheck::PatchXml();
    if (!doc.load_buffer(xml.data(), xml.size())) {
        return extras;
    }
    for (const pugi::xml_node patch : doc.child("Patch").children("Metadata")) {
        const std::string_view name = patch.attribute("Name").value();
        if (IsFrameRatePatch(name) || IsResolutionPatch(patch.attribute("Category").value()) ||
            name == DlcPatch || name == LooseFilesPatch) {
            continue;
        }
        extras.push_back({std::string(name), patch.attribute("isEnabled").as_bool()});
    }
    return extras;
}

bool IsEnabled(std::string_view name, std::string_view category, bool listed_on) {
    if (name == LooseFilesPatch) {
        return GameCheck::LooseFiles();
    }
    const Settings::Game game = Settings::Get().game;
    if (IsFrameRatePatch(name)) {
        return name == FrameRatePatch(game.frame_rate);
    }
    if (IsResolutionPatch(category)) {
        const Aspect& aspect = ChosenAspect(game.aspect);
        return category == aspect.category && IsChosenSize(name, aspect, game.render_size);
    }
    if (name == DlcPatch) {
        return game.dlc_entitlements;
    }
    const auto extra = game.extras.find(std::string(name));
    return extra != game.extras.end() ? extra->second : listed_on;
}

void ApplySettings() {
    const u32 fps = Settings::Get().game.frame_rate;
    // The game's own 30 frames run with the PS4's 60 Hz vblank.
    EmulatorSettings.SetVblankFrequency(FrameRatePatch(fps).empty() ? 60 : fps, true);
    const int dmem = ResolutionDmem();
    if (dmem > EmulatorSettings.GetExtraDmemInMBytes()) {
        EmulatorSettings.SetExtraDmemInMBytes(dmem, true);
    }
    EmulatorSettings.SetConnectedToNetwork(true, true);
    EmulatorSettings.SetShadNetEnabled(true, true);
    EmulatorSettings.SetShadNetServer(std::string(ShadNetServer), true);
    EmulatorSettings.SetShadNetWebApiServer(std::string(ShadNetWebApiServer), true);
    EmulatorSettings.SetP2PPort(P2pPort, true);
}

float AspectRatio() {
    return ChosenAspect(Settings::Get().game.aspect).ratio;
}

bool HalloweenItems() {
    return Settings::Get().game.halloween_items;
}

} // namespace Gr2Port::GamePatches
