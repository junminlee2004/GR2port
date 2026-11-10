// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <span>
#include <string>
#include <vector>
#include <SDL3/SDL_messagebox.h>
#include <cmrc/cmrc.hpp>
#include <fmt/format.h>
#include <pugixml.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/file_format/psf.h"
#include "core/file_sys/ifile.h"
#include "core/game_check/game_check.h"
#include "core/language/language.h"
#include "core/loader/elf.h"

CMRC_DECLARE(gr2port_patches);

namespace Gr2Port::GameCheck {

namespace {

using Core::FileSys::IBackend;

constexpr const char* PatchFile = "src/resources/patches/GravityDaze2-Orbis.xml";
constexpr std::string_view Version = "01.11";
constexpr std::array UpdateSuffixes{"-patch", "-UPDATE"};
constexpr std::array Archives{"Gravite2_Chunk0", "Gravite2_Chunk1", "Gravite2_Patch_Chunk0",
                              "Gravite2_Patch_Chunk1"};
constexpr u64 PatchBase = 0x400000;

constexpr std::string_view PkgNote =
    "If your game dump is in .pkg format, please extract it beforehand.";

struct Expected {
    u64 address;
    std::string_view bytes;
};

// The v1.11 eboot.bin's bytes where some of the built-in patches write, at the patches' addresses,
// read from a good dump. The patches fit only an eboot.bin that holds them.
constexpr std::array ExpectedBytes{
    Expected{0x0137a6d8, "75"},
    Expected{0x0137a738, "488b3d31529600"},
    Expected{0x0137b041, "0f84aaf7ffff"},
    Expected{0x01377bbc, "75"},
    Expected{0x01377c1c, "488b3d4d7d9600"},
    Expected{0x0143e71f, "e81c8c1d00"},
    Expected{0x0143ea18, "e823891d00"},
    Expected{0x014365f4, "7309"},
    Expected{0x01c320c4, "820e0e36"},
    Expected{0x01c3220c, "820c0c36"},
    Expected{0x01c32354, "820c0c36"},
    Expected{0x01c32498, "82101036"},
    Expected{0x01c3266c, "f6020c7e"},
    Expected{0x01c32761, "00"},
    Expected{0x01c33bd9, "00"},
    Expected{0x0142dea0, "bafe010000"},
    Expected{0x0142deb7, "bafe010000"},
    Expected{0x0143ef3d, "83c20783c307c1ea03c1eb030fafda"},
    Expected{0x00a317cc, "c5fa58059883e800"},
    Expected{0x01c1b3d8, "6f82611400000000"},
    Expected{0x008501da, "2f617263"},
    Expected{0x0086c7da, "2f617263"},
    Expected{0x0086c8b6, "2f617263"},
    Expected{0x0086cb23, "2f617263"},
    Expected{0x0086cbdc, "2f617263"},
    Expected{0x0086cfca, "2f617263"},
    Expected{0x014ce352, "2f617263"},
    Expected{0x014e0b7a, "2f617263"},
    Expected{0x01515f92, "2f617263"},
    Expected{0x0151caf1, "2f617263"},
    Expected{0x018ce7de, "2f617263"},
};

enum class Form {
    Missing,
    Archive,
    Folder,
};

std::atomic<bool> g_loose_files{false};
// Set by Find before anything reads it.
std::string g_title_id;

// English text with fmt's {} markers in the player's language, filled in. A translation that does
// not take the values falls back to the English.
template <typename... Args>
std::string Say(std::string_view english, const Args&... args) {
    try {
        return fmt::format(fmt::runtime(Language::Text(english)), args...);
    } catch (const fmt::format_error&) {
        return fmt::format(fmt::runtime(english), args...);
    }
}

void Tell(const std::string& text) {
    LOG_ERROR(Loader, "GR2port: {}", text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gravity Rush 2", text.c_str(), nullptr);
}

// The user folder is beside the executable, or beside the app bundle or AppImage that holds it.
std::filesystem::path ExecutableFolder() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir).parent_path();
}

// The title IDs of the regions the patches fit, from the patch file's own list.
std::vector<std::string> TitleIds() {
    std::vector<std::string> ids;
    pugi::xml_document doc;
    const std::string_view xml = PatchXml();
    if (doc.load_buffer(xml.data(), xml.size())) {
        for (const pugi::xml_node id : doc.child("Patch").child("TitleID").children("ID")) {
            ids.emplace_back(id.text().get());
        }
    }
    return ids;
}

std::optional<std::string> AppVersion(IBackend& files) {
    const std::optional<std::vector<u8>> bytes = files.ReadFile("sce_sys/param.sfo");
    PSF psf;
    if (!bytes || !psf.Open(*bytes)) {
        return std::nullopt;
    }
    const std::optional<std::string_view> version = psf.GetString("APP_VER");
    return version ? std::optional<std::string>{*version} : std::nullopt;
}

std::vector<u8> FromHex(std::string_view hex) {
    std::vector<u8> bytes(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, bytes[i], 16);
    }
    return bytes;
}

// Reads eboot.bin at each expected address through its program headers, as the loader would.
bool EbootFits(IBackend& files) {
    std::unique_ptr<Core::FileSys::IFile> file =
        files.Open("eboot.bin", Common::FS::FileAccessMode::Read);
    if (file == nullptr) {
        return false;
    }
    Core::Loader::Elf elf;
    elf.Open(std::move(file));
    if (!elf.IsElfFile()) {
        return false;
    }
    const std::span<const elf_program_header> headers = elf.GetProgramHeader();
    for (const Expected& expected : ExpectedBytes) {
        const std::vector<u8> wanted = FromHex(expected.bytes);
        const u64 address = expected.address - PatchBase;
        const auto segment = std::ranges::find_if(headers, [&](const auto& header) {
            return header.p_type == PT_LOAD && address >= header.p_vaddr &&
                   address + wanted.size() <= header.p_vaddr + header.p_filesz;
        });
        if (segment == headers.end()) {
            return false;
        }
        std::vector<u8> found(wanted.size());
        if (!elf.TryLoadSegment(reinterpret_cast<u64>(found.data()),
                                segment->p_offset + address - segment->p_vaddr, found.size()) ||
            found != wanted) {
            return false;
        }
    }
    return true;
}

// The game reads the update over the game, so either may hold an archive. An archive beside its
// own extracted folder counts as the archive.
Form FormOf(std::string_view name, std::initializer_list<IBackend*> roots) {
    const std::string archive = std::string(name) + ".psarc";
    bool folder = false;
    for (IBackend* const files : roots) {
        if (files == nullptr) {
            continue;
        }
        if (files->Exists(archive) && !files->IsDirectory(archive)) {
            return Form::Archive;
        }
        folder = folder || files->IsDirectory(name);
    }
    return folder ? Form::Folder : Form::Missing;
}

std::string NoGame(const std::filesystem::path& folder) {
    return Say(
        "No CUSAXXXXX and/or CUSAXXXXX-patch folder found in\n{}\n\nPut Gravity Rush 2 and its "
        "1.11 update beside Gravity Rush 2\xE2\x84\xA2"
        ".exe: either two folders, the game and the update (for example CUSA04934 and "
        "CUSA04934-patch), or one folder with the update copied over the game. Folders and .zar "
        "archives both work.\n\n{}",
        Common::FS::PathToUTF8String(folder), Language::Text(PkgNote));
}

std::optional<Game> Check(const std::string& id, const std::optional<std::filesystem::path>& root,
                          const std::optional<std::filesystem::path>& update_root) {
    const std::string update_name = id + "-patch";
    if (!root) {
        Tell(Say("No {} folder found beside {}. GR2port needs the game as well as its update: "
                 "put the game beside it as {}, or copy the update over the game folder.\n\n{}",
                 id, update_name, id, Language::Text(PkgNote)));
        return std::nullopt;
    }
    const std::unique_ptr<IBackend> game = Core::FileSys::OpenGameBackend(*root);
    const std::unique_ptr<IBackend> update =
        update_root ? Core::FileSys::OpenGameBackend(*update_root) : nullptr;
    if (game == nullptr || (update_root && update == nullptr)) {
        Tell(Say("{} cannot be opened. Your game files may be corrupted.",
                 update_root && update == nullptr ? update_name : id));
        return std::nullopt;
    }

    // Without an update beside the game, the update is copied over it and its version shows it.
    if (update == nullptr) {
        if (const auto version = AppVersion(*game); version && *version != Version) {
            Tell(Say("No {} folder found. {} holds version {} of the game, and GR2port needs "
                     "the 1.11 update: put it beside the game as {}, or copy it over the game "
                     "folder.\n\n{}",
                     update_name, id, *version, update_name, Language::Text(PkgNote)));
            return std::nullopt;
        }
    } else if (const auto version = AppVersion(*update); version && *version != Version) {
        Tell(Say("The update in {} is version {}. GR2port needs Gravity Rush 2 update 1.11.",
                 update_name, *version));
        return std::nullopt;
    }

    IBackend* eboot = nullptr;
    if (update != nullptr && update->Exists("eboot.bin")) {
        eboot = update.get();
    } else if (game->Exists("eboot.bin")) {
        eboot = game.get();
    }
    if (eboot == nullptr) {
        Tell(Say("No eboot.bin found. Your game files may be corrupted."));
        return std::nullopt;
    }
    if (!EbootFits(*eboot)) {
        Tell(Say("Your game files are corrupted: eboot.bin in {} is not the Gravity Rush 2 "
                 "v1.11 eboot.bin that GR2port's patches fit.",
                 eboot == update.get() ? update_name : id));
        return std::nullopt;
    }

    constexpr std::array FormNames{"missing", ".psarc archive", "extracted folder"};
    std::array<Form, Archives.size()> forms{};
    std::string listing;
    for (size_t i = 0; i < Archives.size(); ++i) {
        forms[i] = FormOf(Archives[i], {update.get(), game.get()});
        listing += fmt::format("\n{}: {}", Archives[i],
                               Language::Text(FormNames[static_cast<size_t>(forms[i])]));
    }
    const auto all = [&forms](Form form) {
        return std::ranges::all_of(forms, [form](Form found) { return found == form; });
    };
    if (!all(Form::Archive) && !all(Form::Folder)) {
        Tell(Say("Bad dump: the game's four archives must all be .psarc files, or all be "
                 "extracted to folders of the same names.\n{}",
                 listing));
        return std::nullopt;
    }
    const bool loose_files = all(Form::Folder);
    g_loose_files.store(loose_files, std::memory_order_relaxed);
    g_title_id = id;
    LOG_INFO(Loader, "GR2port: Gravity Rush 2 v1.11 {} in {}, {}", id,
             Common::FS::PathToUTF8String(*root), loose_files ? "loose files" : "archives");
    return Game{*root, loose_files};
}

} // namespace

std::optional<Game> Find() {
    const std::filesystem::path folder = ExecutableFolder();
    for (const std::string& id : TitleIds()) {
        const std::optional<std::filesystem::path> root =
            Core::FileSys::ResolveGameRoot(folder / id);
        std::optional<std::filesystem::path> update;
        for (const char* const suffix : UpdateSuffixes) {
            if (!update) {
                update = Core::FileSys::ResolveGameRoot(folder / (id + suffix));
            }
        }
        if (root || update) {
            return Check(id, root, update);
        }
    }
    Tell(NoGame(folder));
    return std::nullopt;
}

bool LooseFiles() {
    return g_loose_files.load(std::memory_order_relaxed);
}

std::string TitleId() {
    return g_title_id;
}

std::string_view PatchXml() {
    const cmrc::file file = cmrc::gr2port_patches::get_filesystem().open(PatchFile);
    return std::string_view{file.begin(), file.size()};
}

} // namespace Gr2Port::GameCheck
