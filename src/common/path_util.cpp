// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "common/types.h"
#include "core/file_sys/ifile.h"

#ifdef __APPLE__
#include <CoreFoundation/CFBundle.h>
#include <dlfcn.h>
#include <sys/param.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef MAX_PATH
// This is the maximum number of UTF-8 code units permissible in all other OSes' file paths
#define MAX_PATH 1024
#endif

namespace Common::FS {

namespace fs = std::filesystem;

#ifdef __APPLE__
using IsTranslocatedURLFunc = Boolean (*)(CFURLRef path, bool* isTranslocated,
                                          CFErrorRef* __nullable error);
using CreateOriginalPathForURLFunc = CFURLRef __nullable (*)(CFURLRef translocatedPath,
                                                             CFErrorRef* __nullable error);

static CFURLRef UntranslocateBundlePath(const CFURLRef bundle_path) {
    if (void* security_handle =
            dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY)) {
        SCOPE_EXIT {
            dlclose(security_handle);
        };

        const auto IsTranslocatedURL = reinterpret_cast<IsTranslocatedURLFunc>(
            dlsym(security_handle, "SecTranslocateIsTranslocatedURL"));
        const auto CreateOriginalPathForURL = reinterpret_cast<CreateOriginalPathForURLFunc>(
            dlsym(security_handle, "SecTranslocateCreateOriginalPathForURL"));

        bool is_translocated = false;
        if (IsTranslocatedURL && CreateOriginalPathForURL &&
            IsTranslocatedURL(bundle_path, &is_translocated, nullptr) && is_translocated) {
            return CreateOriginalPathForURL(bundle_path, nullptr);
        }
    }
    return nullptr;
}

// The app bundle that holds the executable, or the folder that does when there is none.
static std::optional<std::filesystem::path> GetBundleDirectory() {
    if (CFBundleRef bundle_ref = CFBundleGetMainBundle()) {
        if (CFURLRef bundle_url_ref = CFBundleCopyBundleURL(bundle_ref)) {
            SCOPE_EXIT {
                CFRelease(bundle_url_ref);
            };

            CFURLRef untranslocated_url_ref = UntranslocateBundlePath(bundle_url_ref);
            SCOPE_EXIT {
                if (untranslocated_url_ref) {
                    CFRelease(untranslocated_url_ref);
                }
            };

            char app_bundle_path[MAXPATHLEN];
            if (CFURLGetFileSystemRepresentation(
                    untranslocated_url_ref ? untranslocated_url_ref : bundle_url_ref, true,
                    reinterpret_cast<u8*>(app_bundle_path), sizeof(app_bundle_path))) {
                return std::filesystem::path{app_bundle_path};
            }
        }
    }
    return std::nullopt;
}
#endif

// The folder that holds the executable, on macOS the one that holds its app bundle, and on Linux
// the one that holds its AppImage.
static fs::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return fs::current_path();
        }
        if (length < path.size()) {
            path.resize(length);
            return fs::path{path}.parent_path();
        }
        path.resize(path.size() * 2);
    }
#elif defined(__APPLE__)
    if (const auto bundle = GetBundleDirectory()) {
        return bundle->extension() == ".app" ? bundle->parent_path() : *bundle;
    }
    return fs::current_path();
#else
    // An AppImage runs from a read-only mount, so the folder that holds the AppImage counts.
    if (const char* const appimage = std::getenv("APPIMAGE")) {
        return fs::path{appimage}.parent_path();
    }
    std::error_code ec;
    const auto executable = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : executable.parent_path();
#endif
}

static auto UserPaths = [] {
    // GR2port keeps every file of its own in userdata beside the executable, made if missing.
    const auto user_dir = ExecutableDirectory() / PORTABLE_DIR;

    std::unordered_map<PathType, fs::path> paths;

    const auto create_path = [&](PathType shad_path, const fs::path& new_path) {
        std::filesystem::create_directory(new_path);
        paths.insert_or_assign(shad_path, new_path);
    };

    create_path(PathType::UserDir, user_dir);
    create_path(PathType::LogDir, user_dir / LOG_DIR);
    create_path(PathType::ScreenshotsDir, user_dir / SCREENSHOTS_DIR);
    create_path(PathType::ShaderDir, user_dir / SHADER_DIR);
    create_path(PathType::GameDataDir, user_dir / GAMEDATA_DIR);
    create_path(PathType::SysModuleDir, user_dir / SYSMODULES_DIR);
    create_path(PathType::CapturesDir, user_dir / CAPTURES_DIR);
    create_path(PathType::CacheDir, user_dir / CACHE_DIR);
    create_path(PathType::TrophyDir, user_dir / TROPHY_DIR);
    create_path(PathType::SaveDataDir, user_dir / SAVEDATA_DIR);

    return paths;
}();

bool ValidatePath(const fs::path& path) {
    if (path.empty()) {
        LOG_ERROR(Common_Filesystem, "Input path is empty, path={}", PathToUTF8String(path));
        return false;
    }

#ifdef _WIN32
    if (path.u16string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#else
    if (path.u8string().size() >= MAX_PATH) {
        LOG_ERROR(Common_Filesystem, "Input path is too long, path={}", PathToUTF8String(path));
        return false;
    }
#endif

    return true;
}

std::string PathToUTF8String(const std::filesystem::path& path) {
    const auto u8_string = path.u8string();
    return std::string{u8_string.begin(), u8_string.end()};
}

const fs::path& GetUserPath(PathType shad_path) {
    return UserPaths.at(shad_path);
}

std::string GetUserPathString(PathType shad_path) {
    return PathToUTF8String(GetUserPath(shad_path));
}

void SetUserPath(PathType shad_path, const fs::path& new_path) {
    if (!std::filesystem::is_directory(new_path)) {
        LOG_ERROR(Common_Filesystem, "Filesystem object at new_path={} is not a directory",
                  PathToUTF8String(new_path));
        return;
    }

    UserPaths.insert_or_assign(shad_path, new_path);
}

std::optional<fs::path> FindGameByID(const fs::path& dir, const std::string& game_id,
                                     int max_depth) {
    const auto boot_path_for = [](const fs::path& root) -> std::optional<fs::path> {
        std::error_code ec;
        if (fs::is_directory(root, ec) && !ec) {
            if (!fs::exists(root / "sce_sys" / "param.sfo")) {
                return std::nullopt;
            }
            if (auto eboot_path = root / "eboot.bin"; fs::exists(eboot_path)) {
                return eboot_path;
            }
            return std::nullopt;
        }
        if (Core::FileSys::IsZArchiveFile(root) &&
            Core::FileSys::ReadGameFile(root, "sce_sys/param.sfo").has_value()) {
            return root;
        }
        return std::nullopt;
    };

    bool is_different_game_root = false;

    // Check if this is a valid game root
    if (auto found = boot_path_for(dir)) {
        if (dir.filename() == game_id) { // Check if this is the game we're looking for
            return found;
        } else { // if not, don't waste time looking through game data folders in a different game
            is_different_game_root = true;
        }
    }

    if (is_different_game_root) {
        return std::nullopt;
    } else {
        if (auto found = boot_path_for(dir / game_id)) {
            return found;
        }
        if (auto found = boot_path_for(dir / (game_id + ".zar"))) {
            return found;
        }
    }

    if (max_depth <= 0) {
        return std::nullopt;
    }

    // Recursively search subdirectories
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        std::error_code entry_ec;
        if (!entry.is_directory(entry_ec) || entry_ec) {
            continue;
        }
        if (auto found = FindGameByID(entry.path(), game_id, max_depth - 1)) {
            return found;
        }
    }

    return std::nullopt;
}

} // namespace Common::FS
