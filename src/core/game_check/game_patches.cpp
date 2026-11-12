// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <charconv>
#include <span>
#include <string>
#include <fmt/format.h>
#include <pugixml.hpp>

#include "common/memory_patcher.h"
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

// The game's level of detail distances, in metres: its scene starts with these two rows of the
// executable, Level0 to Level7 each. A model at or past Level i of its row draws its meshes of
// level i + 1. The world's models take the first row; the characters' take the second, whose
// Level3 to Level7 no distance reaches.
constexpr u64 WorldDetailRow = 0x018af120;
constexpr u64 CharacterDetailRow = 0x018af140;
constexpr std::array WorldDetail{0.0f, 35.0f, 55.0f, 80.0f, 110.0f, 145.0f, 185.0f, 230.0f};
constexpr std::array CharacterDetail{0.0f, 15.0f, 25.0f};

// An area of the world, an island of the city or a part of one, shows its detailed models while
// the player is inside its box grown by a margin, and its low model outside. The margins are nine
// floats of the executable, three for each kind of area that lua/area/areaAttrs.evd names enter,
// dummyonly and root: the box's scale, the margin in metres and the margin in Hekseville. At its
// start the game stores the file's values over them, each kind with three instructions of eight
// bytes: AreaMarginStores. AreaMarginsOfFile are that file's values.
constexpr u64 AreaMargins = 0x01c80a70;
constexpr std::array<u64, 3> AreaMarginStores{0x0098284e, 0x00982881, 0x009828a5};
constexpr std::array AreaMarginsOfFile{1.0f,  80.0f, 50.0f,  1.0f,  100.0f,
                                       70.0f, 1.0f,  130.0f, 110.0f};
constexpr std::string_view ThreeStoresSkipped = "0f1f8400000000000f1f8400000000000f1f840000000000";

// Queues a patch of the bytes, as hexadecimal digits, at address.
void QueueBytes(u64 address, std::string bytes) {
    MemoryPatcher::AddPatchToQueue({
        .gameSerial = "*",
        .modNameStr = "Level of detail",
        .offsetStr = fmt::format("{:#x}", address),
        .valueStr = std::move(bytes),
        .isOffset = false,
        .littleEndian = false,
        .patchMask = MemoryPatcher::PatchMask::None,
        .maskOffset = 0,
    });
}

std::string FloatBytes(float value) {
    const u32 bits = std::bit_cast<u32>(value);
    return fmt::format("{:02x}{:02x}{:02x}{:02x}", bits & 0xff, bits >> 8 & 0xff, bits >> 16 & 0xff,
                       bits >> 24);
}

// Queues the patch that writes a row's distances at percent of the game's; 100 keeps the row.
void QueueDetail(u64 row, std::span<const float> distances, u32 percent) {
    if (percent == 100) {
        return;
    }
    std::string bytes;
    for (const float distance : distances) {
        bytes += FloatBytes(distance * static_cast<float>(percent) / 100.0f);
    }
    QueueBytes(row, std::move(bytes));
}

// Above 100 percent, queues the patches that grow the areas' margins to percent of the file's, so
// an area shows its detailed models from further away. The game's stores of the file's values are
// skipped and the grown values written in their place. At 100 and below the margins stay the
// file's: smaller ones leave the game less time to load an area before the player reaches it.
void QueueAreaMargins(u32 percent) {
    if (percent <= 100) {
        return;
    }
    for (const u64 stores : AreaMarginStores) {
        QueueBytes(stores, std::string(ThreeStoresSkipped));
    }
    std::string bytes;
    for (size_t i = 0; i < AreaMarginsOfFile.size(); ++i) {
        const bool scale = i % 3 == 0;
        bytes += FloatBytes(scale ? AreaMarginsOfFile[i]
                                  : AreaMarginsOfFile[i] * static_cast<float>(percent) / 100.0f);
    }
    QueueBytes(AreaMargins, std::move(bytes));
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
    const Settings::Graphics graphics = Settings::Get().graphics;
    QueueAreaMargins(graphics.far_environment_detail);
    QueueDetail(WorldDetailRow, WorldDetail, graphics.near_environment_detail);
    QueueDetail(CharacterDetailRow, CharacterDetail, graphics.character_detail);
}

float AspectRatio() {
    return ChosenAspect(Settings::Get().game.aspect).ratio;
}

bool HalloweenItems() {
    return Settings::Get().game.halloween_items;
}

} // namespace Gr2Port::GamePatches
