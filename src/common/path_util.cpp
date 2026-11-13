// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
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
// windows.h first
#include <shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
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

fs::path GetExecutableFile() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return fs::path{path};
        }
        path.resize(path.size() * 2);
    }
#elif defined(__APPLE__)
    return {};
#else
    // An AppImage runs from a read-only mount, so the AppImage counts.
    if (const char* const appimage = std::getenv("APPIMAGE")) {
        return fs::path{appimage};
    }
    std::error_code ec;
    const auto executable = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : executable;
#endif
}

fs::path GetExecutableDirectory() {
#ifdef __APPLE__
    if (const auto bundle = GetBundleDirectory()) {
        return bundle->extension() == ".app" ? bundle->parent_path() : *bundle;
    }
    return fs::current_path();
#else
    const fs::path file = GetExecutableFile();
    return file.empty() ? fs::current_path() : file.parent_path();
#endif
}

fs::path GetGlobalUserFolder() {
#ifdef _WIN32
    wchar_t appdata[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    return appdata[0] != L'\0' ? fs::path{appdata} / GLOBAL_DIR : fs::path{};
#elif defined(__APPLE__)
    const char* const home = std::getenv("HOME");
    return home ? fs::path{home} / "Library" / "Application Support" / GLOBAL_DIR : fs::path{};
#else
    const char* const data = std::getenv("XDG_DATA_HOME");
    if (data != nullptr && data[0] != '\0') {
        return fs::path{data} / GLOBAL_DIR;
    }
    const char* const home = std::getenv("HOME");
    return home ? fs::path{home} / ".local" / "share" / GLOBAL_DIR : fs::path{};
#endif
}

// The file beside the executable that tells the user folder is the global one.
constexpr auto GLOBAL_MARK = "global_userdata.txt";
// RequestUserFolderMode's word to the executable's next start, in the environment.
constexpr auto MODE_REQUEST = "GR2PORT_USER_FOLDER";
constexpr auto GLOBAL_REQUEST = "global";
constexpr auto PORTABLE_REQUEST = "portable";
// The process RestartExecutable ran in, which the one it started waits for on Windows.
constexpr auto RESTART_PROCESS = "GR2PORT_RESTART_PID";

static bool g_global_user_folder = false;
static UserFolderSwitch g_user_folder_switch = UserFolderSwitch::None;

// Sets a variable of the environment. An empty value removes it.
static void SetEnvironment(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value[0] != '\0') {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

// Moves a folder to a place that holds nothing, by a copy when the place is on another drive.
static bool MoveFolder(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(to, ec);
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

// Whether the user folder is the global one, which the mark tells. A request for the other mode
// is carried out first: the folder moves to the mode's place unless a folder is there already.
static bool UseGlobalFolder(const fs::path& portable, const fs::path& global) {
    const fs::path mark = GetExecutableDirectory() / GLOBAL_MARK;
    std::error_code ec;
    const bool is_global = fs::exists(mark, ec);
    const char* const request = std::getenv(MODE_REQUEST);
    if (request == nullptr) {
        return is_global;
    }
    const bool to_global = std::string_view{request} == GLOBAL_REQUEST;
    SetEnvironment(MODE_REQUEST, "");
    if (to_global == is_global) {
        return is_global;
    }
    const fs::path& from = to_global ? portable : global;
    const fs::path& to = to_global ? global : portable;
    if (fs::exists(to, ec)) {
        g_user_folder_switch = UserFolderSwitch::Kept;
    } else if (!fs::exists(from, ec)) {
        // Nothing moves: the folder is made new, as at any start.
        g_user_folder_switch = to_global ? UserFolderSwitch::None : UserFolderSwitch::GlobalMissing;
    } else if (!MoveFolder(from, to)) {
        g_user_folder_switch = UserFolderSwitch::Failed;
        return is_global;
    }
    if (to_global) {
        std::ofstream{mark} << "GR2port keeps its files in the " << GLOBAL_DIR
                            << " folder of this account's application data. The Paths tab of "
                               "the launcher's Options brings them back beside the game.\n";
    } else {
        fs::remove(mark, ec);
    }
    return to_global;
}

static auto UserPaths = [] {
#ifdef _WIN32
    // The run that restarted the executable holds the user folder until it ends.
    if (const char* const process = std::getenv(RESTART_PROCESS)) {
        if (const HANDLE handle =
                OpenProcess(SYNCHRONIZE, FALSE, std::strtoul(process, nullptr, 10))) {
            WaitForSingleObject(handle, INFINITE);
            CloseHandle(handle);
        }
        SetEnvironment(RESTART_PROCESS, "");
    }
#endif
    // GR2port keeps every file of its own in one user folder, made if missing: userdata beside
    // the executable or, in the global mode, GR2port in the account's application data.
    const fs::path portable = GetExecutableDirectory() / PORTABLE_DIR;
    const fs::path global = GetGlobalUserFolder();
    g_global_user_folder = UseGlobalFolder(portable, global);
    const auto user_dir = g_global_user_folder ? global : portable;

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
    create_path(PathType::CapturesDir, user_dir / CAPTURES_DIR);
    create_path(PathType::CacheDir, user_dir / CACHE_DIR);
    create_path(PathType::TrophyDir, user_dir / TROPHY_DIR);
    create_path(PathType::SaveDataDir, user_dir / SAVEDATA_DIR);

    return paths;
}();

UserFolderMode GetUserFolderMode() {
    return g_global_user_folder ? UserFolderMode::Global : UserFolderMode::Portable;
}

UserFolderSwitch GetUserFolderSwitch() {
    return g_user_folder_switch;
}

void RequestUserFolderMode(UserFolderMode mode) {
    SetEnvironment(MODE_REQUEST,
                   mode == UserFolderMode::Global ? GLOBAL_REQUEST : PORTABLE_REQUEST);
}

bool RestartExecutable() {
#ifdef _WIN32
    SetEnvironment(RESTART_PROCESS, std::to_string(GetCurrentProcessId()).c_str());
    std::wstring arguments = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(GetExecutableFile().c_str(), arguments.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#elif defined(__linux__)
    // The kernel lists this run's arguments, each ended by a zero. The first names the program,
    // which inside an AppImage is not the file to start.
    std::ifstream file{"/proc/self/cmdline", std::ios::binary};
    std::string line{std::istreambuf_iterator<char>{file}, {}};
    std::string executable = GetExecutableFile().string();
    std::vector<char*> arguments{executable.data()};
    for (size_t at = std::strlen(line.c_str()) + 1; at < line.size();
         at += std::strlen(&line[at]) + 1) {
        arguments.push_back(&line[at]);
    }
    arguments.push_back(nullptr);
    execv(executable.c_str(), arguments.data());
    return false;
#else
    return false;
#endif
}

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
