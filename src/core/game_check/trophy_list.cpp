// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <map>
#include <span>
#include <string_view>
#include <system_error>
#include <fmt/format.h>
#include <pugixml.hpp>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/file_format/npbind.h"
#include "core/file_format/trp.h"
#include "core/game_check/game_check.h"
#include "core/game_check/trophy_list.h"

namespace Gr2Port::Trophies {

namespace {

// A time later than this is not in seconds.
constexpr u64 LastSecond = 9'999'999'999;

// The copies of the game's trophy file of an index, under either spelling of its name.
std::vector<std::vector<u8>> TrophyFiles(size_t index) {
    auto copies = GameCheck::Files(fmt::format("sce_sys/trophy/trophy{:02d}.trp", index));
    if (copies.empty()) {
        copies = GameCheck::Files(fmt::format("sce_sys/trophy/TROPHY{:02d}.TRP", index));
    }
    return copies;
}

// Extracts the first copy of a trophy file that extracts. TRP reads a file on disk, so each copy
// goes through a temporary one.
bool ExtractList(size_t index, const std::string& id, const std::filesystem::path& folder) {
    std::error_code ec;
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path(ec) / fmt::format("{}_trophy{:02d}.trp", id, index);
    bool extracted = false;
    for (const std::vector<u8>& bytes : TrophyFiles(index)) {
        {
            Common::FS::IOFile out(temporary, Common::FS::FileAccessMode::Create);
            if (!out.IsOpen() || out.WriteRaw<u8>(bytes.data(), bytes.size()) != bytes.size()) {
                continue;
            }
        }
        TRP trp;
        if (trp.Extract(temporary, id, folder)) {
            extracted = true;
            break;
        }
    }
    std::filesystem::remove(temporary, ec);
    return extracted;
}

// The trophies the player earned in a list, with the time of each, from the progress file.
std::map<s32, u64> Earned(const std::filesystem::path& progress_file) {
    std::map<s32, u64> earned;
    pugi::xml_document progress;
    if (!progress.load_file(progress_file.native().c_str())) {
        return earned;
    }
    for (const pugi::xml_node& node : progress.child("trophyconf").children("trophy")) {
        if (node.attribute("unlockstate").as_bool()) {
            const u64 time = node.attribute("timestamp").as_ullong();
            earned[node.attribute("id").as_int(-1)] = time <= LastSecond ? time : 0;
        }
    }
    return earned;
}

} // namespace

void Extract() {
    const std::vector<std::vector<u8>> bindings = GameCheck::Files("sce_sys/npbind.dat");
    NPBindFile npbind;
    if (bindings.empty() || !npbind.Load(std::span<const u8>{bindings.front()})) {
        LOG_WARNING(Loader, "Trophies: the game's npbind.dat cannot be read");
        return;
    }
    const std::filesystem::path trophies = Common::FS::GetUserPath(Common::FS::PathType::TrophyDir);
    const std::vector<std::string> ids = npbind.GetNpCommIds();
    for (size_t index = 0; index < ids.size(); ++index) {
        std::error_code ec;
        const std::filesystem::path folder = trophies / ids[index];
        if (!std::filesystem::exists(folder, ec) && !ExtractList(index, ids[index], folder)) {
            LOG_ERROR(Loader, "Trophies: trophy file {} cannot be extracted", index);
            continue;
        }
        // The player's trophy progress starts as a copy of the trophy list, beside its folder.
        const std::filesystem::path progress = trophies / (ids[index] + ".xml");
        if (!std::filesystem::exists(progress, ec)) {
            std::filesystem::copy_file(folder / "Xml" / "TROPCONF.XML", progress, ec);
        }
    }
}

std::vector<List> Read(s32 console_language) {
    const std::filesystem::path trophies = Common::FS::GetUserPath(Common::FS::PathType::TrophyDir);
    std::vector<std::filesystem::path> folders;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(trophies, ec)) {
        if (std::filesystem::is_directory(entry.path() / "Xml", ec)) {
            folders.push_back(entry.path());
        }
    }
    std::ranges::sort(folders);

    std::vector<List> lists;
    for (const std::filesystem::path& folder : folders) {
        // The language's own file where the game has one, else the list's main file.
        std::filesystem::path names =
            folder / "Xml" / fmt::format("TROP_{:02d}.XML", console_language);
        if (!std::filesystem::exists(names, ec)) {
            names = folder / "Xml" / "TROP.XML";
        }
        pugi::xml_document document;
        if (!document.load_file(names.native().c_str())) {
            continue;
        }
        std::filesystem::path progress = folder;
        progress += ".xml";
        const std::map<s32, u64> earned = Earned(progress);

        List list;
        const pugi::xml_node conf = document.child("trophyconf");
        list.title = conf.child("title-name").text().as_string();
        for (const pugi::xml_node& node : conf.children("trophy")) {
            Trophy trophy;
            trophy.id = node.attribute("id").as_int(-1);
            if (trophy.id < 0) {
                continue;
            }
            if (const std::string_view grade = node.attribute("ttype").value(); !grade.empty()) {
                trophy.grade = grade.front();
            }
            trophy.hidden = std::string_view{node.attribute("hidden").value()} == "yes";
            trophy.name = node.child("name").text().as_string();
            trophy.detail = node.child("detail").text().as_string();
            trophy.icon = folder / "Icons" / fmt::format("TROP{:03d}.PNG", trophy.id);
            if (const auto time = earned.find(trophy.id); time != earned.end()) {
                trophy.earned = true;
                trophy.earned_at = time->second;
            }
            list.trophies.push_back(std::move(trophy));
        }
        if (!list.trophies.empty()) {
            lists.push_back(std::move(list));
        }
    }
    return lists;
}

} // namespace Gr2Port::Trophies
