// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string_view>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/address_space.h"
#include "core/emulator_settings.h"
#include "core/game_check/game_check.h"
#include "core/recompiled/build.h"

namespace Core::Recompiled::Build {

namespace {

namespace fs = std::filesystem;

// The game's files the generator reads, at their places in the dump: the executable and the
// modules that recompiled mode loads as libraries.
constexpr std::array GameFiles{
    "eboot.bin",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
    "sce_module/libSceJobManager.prx",
    "sce_module/libSceNpToolkit.prx",
    "sysdata/libSceSecure.prx",
};

// What a build of the libraries leaves in its library folder, and the target it builds for.
constexpr std::array Libraries{
    "librecomp.so",           "libc.prx.so",
    "libSceFios2.prx.so",     "libSceJobManager.prx.so",
    "libSceNpToolkit.prx.so", "libSceSecure.prx.so",
    "library.json",
};
constexpr std::string_view Target = "arm64-apple-macos";

// Set by RestartForHookCode for the start it makes ("dumped"), and by that start for its own
// load of the hook code ("built"), which then does not start again.
constexpr const char* RestartVariable = "GR2PORT_HOOK_CODE";

// The generator's tree in the app, Contents/Resources/recompiler, with digest.txt: a hash of all
// of it, which the app's packaging writes.
fs::path Generator() {
    return Common::FS::GetExecutableFile().parent_path().parent_path() / "Resources" / "recompiler";
}

fs::path Builder() {
    return Generator() / "src" / "core" / "recompiled" / "build-library.py";
}

// The generator's work folder: its tools, the objects it compiled and, during a build, the game's
// files and the generated C.
fs::path Work() {
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "recompile";
}

fs::path HookFolder() {
    return Folder() / "hooks";
}

fs::path PendingHookCode() {
    return HookFolder() / "pending";
}

std::string ReadText(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// What the libraries are made from besides the game: the generator's tree, and the address the
// executable loads at, which their code holds as a constant.
std::string Stamp() {
    return fmt::format("{}image base {:#x}\n", ReadText(Generator() / "digest.txt"),
                       ExecutableLoadBase);
}

bool CanBuild() {
    return EmulatorSettings.GetRecompiledLibrary().empty() && fs::exists(Builder()) &&
           fs::exists(Generator() / "digest.txt");
}

bool Ready() {
    for (const char* name : Libraries) {
        if (!fs::exists(Folder() / name)) {
            return false;
        }
    }
    return ReadText(Folder() / "built.txt") == Stamp();
}

std::string Shown(const fs::path& path) {
    return Common::FS::PathToUTF8String(path);
}

// Moves from to to, over a file there, also to another volume.
bool Move(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
        LOG_ERROR(Core, "Recompiled mode: {} could not be put at {}: {}", Shown(from), Shown(to),
                  ec.message());
        return false;
    }
    fs::remove(from, ec);
    return true;
}

// Apple's compiler and Python, of the Command Line Tools, whatever a terminal's PATH names
// first, and no compiled Python files written into the app.
std::vector<std::pair<std::string, std::string>> Environment() {
    return {{"PATH", "/usr/bin:/bin:/usr/sbin:/sbin"}, {"PYTHONDONTWRITEBYTECODE", "1"}};
}

bool ExtractGame(const fs::path& game) {
    std::error_code ec;
    fs::remove_all(game, ec);
    for (const char* path : GameFiles) {
        const std::vector<std::vector<u8>> copies = Gr2Port::GameCheck::Files(path);
        if (copies.empty()) {
            LOG_ERROR(Core, "Recompiled mode: the game has no {}", path);
            return false;
        }
        // The first copy is the update's, which the game runs.
        const fs::path out = game / path;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream file{out, std::ios::binary};
        file.write(reinterpret_cast<const char*>(copies[0].data()),
                   static_cast<std::streamsize>(copies[0].size()));
        if (!file) {
            LOG_ERROR(Core, "Recompiled mode: {} could not be written", Shown(out));
            return false;
        }
    }
    return true;
}

Job LibrariesJob() {
    const fs::path work = Work();
    const fs::path game = work / "game";
    Job job;
    job.title = "Recompiling Gravity Rush 2";
    job.prepare = [game] { return ExtractGame(game); };
    job.program = "/usr/bin/python3";
    job.arguments = {Shown(Builder()),
                     "--game",
                     Shown(game),
                     "--work",
                     Shown(work),
                     "--target",
                     std::string{Target},
                     "--image-base",
                     fmt::format("{:#x}", ExecutableLoadBase),
                     "--progress",
                     "--stop-with-parent"};
    job.environment = Environment();
    job.log = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "recompile.txt";
    job.install = [work, game] {
        std::error_code ec;
        fs::create_directories(Folder(), ec);
        // A stop halfway leaves libraries without a stamp, which the next start builds again.
        fs::remove(Folder() / "built.txt", ec);
        // The hook code's libraries of the last generator: the starts that need them build them.
        fs::remove_all(HookFolder(), ec);
        const fs::path built = work / fmt::format("library-{}", Target);
        for (const char* name : Libraries) {
            if (!Move(built / name, Folder() / name)) {
                return false;
            }
        }
        // What the generator read and wrote, which a later build makes again, goes. The objects
        // stay: that build compiles only the files whose C changed.
        for (const char* made : {"gen", "scan", "modules", "eboot.elf"}) {
            fs::remove_all(work / "build" / made, ec);
        }
        fs::remove_all(game, ec);
        std::ofstream stamp{Folder() / "built.txt", std::ios::binary};
        stamp << Stamp();
        return static_cast<bool>(stamp);
    };
    return job;
}

Job HookCodeJob() {
    const fs::path work = Work();
    Job job;
    job.title = "Recompiling the hook code for these settings";
    job.prepare = [] { return true; };
    job.program = "/usr/bin/python3";
    job.arguments = {Shown(Builder()),    "--hook-code", Shown(PendingHookCode()), "--work",
                     Shown(work),         "--target",    std::string{Target},      "--progress",
                     "--stop-with-parent"};
    job.environment = Environment();
    job.log = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "recompile.txt";
    job.install = [work] {
        // The next free name: 1.so, 2.so and on.
        int number = 1;
        while (fs::exists(HookFolder() / fmt::format("{}.so", number))) {
            ++number;
        }
        if (!Move(work / "hooks" / "hooks.so", HookFolder() / fmt::format("{}.so", number))) {
            return false;
        }
        std::error_code ec;
        fs::remove(PendingHookCode().replace_extension(".bin"), ec);
        fs::remove(PendingHookCode().replace_extension(".txt"), ec);
        return true;
    };
    return job;
}

} // namespace

fs::path Folder() {
    return Common::FS::GetExecutableDirectory() / "recompiled";
}

std::optional<Job> Needed() {
    if (!CanBuild()) {
        return std::nullopt;
    }
    if (!Ready()) {
        return LibrariesJob();
    }
    if (fs::exists(PendingHookCode().replace_extension(".bin")) &&
        fs::exists(PendingHookCode().replace_extension(".txt"))) {
        return HookCodeJob();
    }
    return std::nullopt;
}

std::optional<fs::path> HookDump() {
    const char* const restart = std::getenv(RestartVariable);
    if (!CanBuild() || (restart != nullptr && std::string_view{restart} == "built")) {
        return std::nullopt;
    }
    std::error_code ec;
    fs::create_directories(HookFolder(), ec);
    return PendingHookCode();
}

void RestartForHookCode() {
    setenv(RestartVariable, "dumped", 1);
    LOG_INFO(Core,
             "Recompiled mode: the hook code is saved at {}, and the executable starts "
             "again to build its library",
             Shown(PendingHookCode()));
    Common::Log::Flush();
    Common::FS::RestartExecutable();
    unsetenv(RestartVariable);
    LOG_ERROR(Core, "Recompiled mode: the executable could not start again");
}

bool RestartedForHookCode() {
    const char* const restart = std::getenv(RestartVariable);
    if (restart == nullptr || std::string_view{restart} != "dumped") {
        return false;
    }
    setenv(RestartVariable, "built", 1);
    return true;
}

} // namespace Core::Recompiled::Build
