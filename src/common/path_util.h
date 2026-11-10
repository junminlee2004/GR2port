// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace Common::FS {

enum class PathType {
    UserDir,        // Where GR2port stores its data: userdata beside the executable.
    LogDir,         // Where log files are stored.
    ScreenshotsDir, // Where screenshots are stored.
    ShaderDir,      // Where shaders are stored.
    GameDataDir,    // Where game data is stored.
    SysModuleDir,   // Where system modules are stored.
    CapturesDir,    // Where rdoc captures are stored.
    CacheDir,       // Where pipeline and shader cache is stored.
    TrophyDir,      // Where trophy metadata and the player's trophy progress are stored.
    SaveDataDir,    // Where the game's saves are stored.
};

constexpr auto PORTABLE_DIR = "userdata";

// Sub-directories contained within a user data directory
constexpr auto LOG_DIR = "log";
constexpr auto SCREENSHOTS_DIR = "screenshots";
constexpr auto SHADER_DIR = "shader";
constexpr auto GAMEDATA_DIR = "data";
constexpr auto SYSMODULES_DIR = "sys_modules";
constexpr auto CAPTURES_DIR = "captures";
constexpr auto CACHE_DIR = "cache";
constexpr auto TROPHY_DIR = "trophy";
constexpr auto SAVEDATA_DIR = "savedata";

// Filenames
constexpr auto LOG_FILE = "shad_log.txt";

/**
 * Validates a given path.
 *
 * A given path is valid if it meets these conditions:
 * - The path is not empty
 * - The path is not too long
 *
 * @param path Filesystem path
 *
 * @returns True if the path is valid, false otherwise.
 */
[[nodiscard]] bool ValidatePath(const std::filesystem::path& path);

/**
 * Converts a filesystem path to a UTF-8 encoded std::string.
 *
 * @param path Filesystem path
 *
 * @returns UTF-8 encoded std::string.
 */
[[nodiscard]] std::string PathToUTF8String(const std::filesystem::path& path);

/**
 * Gets the filesystem path associated with the PathType enum.
 *
 * @param user_path PathType enum
 *
 * @returns The filesystem path associated with the PathType enum.
 */
[[nodiscard]] const std::filesystem::path& GetUserPath(PathType user_path);

/**
 * Gets the filesystem path associated with the PathType enum as a UTF-8 encoded std::string.
 *
 * @param user_path PathType enum
 *
 * @returns The filesystem path associated with the PathType enum as a UTF-8 encoded std::string.
 */
[[nodiscard]] std::string GetUserPathString(PathType user_path);

/**
 * Sets a new filesystem path associated with the PathType enum.
 * If the filesystem object at new_path is not a directory, this function will not do anything.
 *
 * @param user_path PathType enum
 * @param new_path New filesystem path
 */
void SetUserPath(PathType user_path, const std::filesystem::path& new_path);

/**
 * Recursively searches for an installed game by its ID.
 * Limits search depth to prevent excessive filesystem traversal.
 *
 * @param dir Base directory to start the search from
 * @param game_id The game ID to search for
 * @param max_depth Maximum directory depth to search
 *
 * @returns A path to eboot.bin for an unpacked game, or the ".zar" archive itself for a
 *          packed one. std::nullopt if the game isn't found.
 */
[[nodiscard]] std::optional<std::filesystem::path> FindGameByID(const std::filesystem::path& dir,
                                                                const std::string& game_id,
                                                                int max_depth);

} // namespace Common::FS
