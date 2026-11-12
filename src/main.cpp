// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <CLI/CLI.hpp>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_messagebox.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "GR2port.h"
#include "common/arch.h"
#include "common/debug_files.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/path_util.h"
#include "core/debugger.h"
#include "core/emulator_settings.h"
#include "core/emulator_state.h"
#include "core/file_sys/fs.h"
#include "core/game_check/game_check.h"
#include "core/ipc/ipc.h"
#include "core/prompt_packs/prompt_packs.h"
#include "core/user_settings.h"

#ifdef _WIN32
#include <windows.h>
#include "launcher/launcher.h"
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include "launcher/launcher.h"
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    Gr2Port::Launcher::AttachTerminal();
    SetConsoleOutputCP(CP_UTF8);
#endif

    // -nolauncher starts the game at once. CLI11 takes no long names after a single dash.
    [[maybe_unused]] bool noLauncher = false;
    for (int i = 1; i < argc && std::string(argv[i]) != "--"; i++) {
        if (std::string(argv[i]) == "-nolauncher") {
            noLauncher = true;
            std::copy(argv + i + 1, argv + argc, argv + i);
            argc--;
            i--;
        }
    }

#if defined(__APPLE__) && defined(ARCH_X86_64)
    // KosmicKrisp only supports Apple Silicon. Check that we are not running on an Intel Mac.
    int sysctl_ret = 0;
    size_t sysctl_size = sizeof(sysctl_ret);
    sysctlbyname("sysctl.proc_translated", &sysctl_ret, &sysctl_size, nullptr, 0);
    if (sysctl_ret != 1) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "GR2port",
                                 "GR2port only supports Apple Silicon Macs.", nullptr);
        std::cout << "GR2port only supports Apple Silicon Macs." << std::endl;
        return -1;
    }
#endif

    CLI::App app{"GR2port CLI"};

    // ---- CLI state ----
    std::vector<std::string> gameArgs;
    std::optional<std::filesystem::path> overrideRoot;
    std::optional<int> waitPid;
    bool waitForDebugger = false;

    std::optional<std::string> fullscreenStr;
    bool ignoreGamePatch = false;
    bool showFps = false;
    bool configClean = false;
    bool configGlobal = false;
    bool append_log{};
    bool debugFiles = false;

    std::optional<std::filesystem::path> addGameFolder;
    std::optional<std::filesystem::path> setAddonFolder;
    std::optional<std::string> patchFile;

    std::vector<std::pair<std::filesystem::path, std::string>> mounts;
    static std::vector<std::string> env_vars;

    // ---- Options ----
    app.add_option("-p,--patch", patchFile, "Patch file to apply");
    app.add_flag("-i,--ignore-game-patch", ignoreGamePatch,
                 "Disable automatic loading of game patches");

    app.add_option("-f,--fullscreen", fullscreenStr, "Fullscreen mode (true|false)");

    app.add_option("--override-root", overrideRoot)->check(CLI::ExistingDirectory);

    app.add_flag("--wait-for-debugger", waitForDebugger);
    app.add_option("--wait-for-pid", waitPid);

    app.add_flag("--show-fps", showFps);
    app.add_flag("--config-clean", configClean);
    app.add_flag("--config-global", configGlobal);
    app.add_flag("--log-append", append_log);
    app.add_flag("--debug-files", debugFiles,
                 "Add the developers' files to the log folder, which holds log.txt alone");

    app.add_option("--add-game-folder", addGameFolder)->check(CLI::ExistingDirectory);
    app.add_option("--set-addon-folder", setAddonFolder)->check(CLI::ExistingDirectory);
    app.add_option("--mount", mounts, "Mount source to destination");
    app.add_option("-e,--env", env_vars, "Environment variables to pass to the guest");

    // ---- Capture args after `--` verbatim ----
    app.allow_extras();

    try {
        bool double_dash_found = false;
        int double_dash_index;
        for (int i = 0; i < argc; i++) {
            if (double_dash_found) {
                gameArgs.emplace_back(argv[i]);
            }
            if (!double_dash_found && std::string(argv[i]) == "--") {
                double_dash_found = true;
                double_dash_index = i;
            }
        }

        // If the -- arg is present, only parse args before it
        if (double_dash_found) {
            app.parse(double_dash_index, argv);
        } else {
            app.parse(argc, argv);
        }
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    Common::SetDebugFiles(debugFiles);

    if (waitPid)
        Core::Debugger::WaitForPid(*waitPid);

    // Initialize main log with default config
    Common::Log::Setup(Common::FS::LOG_FILE);

    LOG_INFO(Debug, "Run: {}", fmt::join(std::span(argv, argc), " "));

    // GR2port: Steam hides the real gamepads from the programs it starts, which leaves its own
    // virtual pad, one without motion sensors. SDL lists every gamepad again.
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES, "", SDL_HINT_OVERRIDE);

    IPC::Instance().Init();

    // Load configurations
    EmulatorSettings.Load();
    UserSettings.Load();

    // ---- Utility commands ----
    if (addGameFolder) {
        EmulatorSettings.AddGameInstallDir(*addGameFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Game folder successfully saved.");
        return 0;
    }

    if (setAddonFolder) {
        EmulatorSettings.SetAddonInstallDir(*setAddonFolder);
        EmulatorSettings.Save();
        LOG_INFO(Config, "Addon folder successfully saved.");
        return 0;
    }

    // GR2port starts only Gravity Rush 2 v1.11 from its own folder, and checks the dump each time.
    const std::optional<Gr2Port::GameCheck::Game> game = Gr2Port::GameCheck::Find();
    if (!game) {
        return 1;
    }
    PromptPacks::InstallBundled();

#if defined(_WIN32) || defined(__linux__)
    if (!noLauncher && !IPC::Instance().IsEnabled() && !Gr2Port::Launcher::Run()) {
        return 0;
    }
#endif

    // ---- Apply flags ----
    if (patchFile)
        MemoryPatcher::patch_file = *patchFile;

    if (ignoreGamePatch)
        Core::FileSys::MntPoints::ignore_game_patches = true;

    if (fullscreenStr) {
        if (*fullscreenStr == "true") {
            EmulatorSettings.SetFullScreen(true);
        } else if (*fullscreenStr == "false") {
            EmulatorSettings.SetFullScreen(false);
        } else {
            LOG_ERROR(Debug, "Invalid argument for --fullscreen (use true|false)");
            return 1;
        }
    }

    if (showFps)
        EmulatorSettings.SetShowFpsCounter(true);

    if (configClean)
        EmulatorSettings.SetConfigMode(ConfigMode::Clean);

    if (configGlobal)
        EmulatorSettings.SetConfigMode(ConfigMode::Global);

    auto* emulator = Common::Singleton<Core::Emulator>::Instance();
#ifdef _WIN32
    // argv is in the ANSI code page, and restarts read the name as UTF-8.
    std::wstring module(32768, L'\0');
    module.resize(GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size())));
    static const std::string executable = Common::FS::PathToUTF8String(module);
    emulator->executableName = executable.c_str();
#else
    emulator->executableName = argv[0];
#endif
    emulator->waitForDebuggerBeforeRun = waitForDebugger;
    emulator->Run(game->root, gameArgs, overrideRoot, mounts, env_vars, append_log);

    return 0;
}
