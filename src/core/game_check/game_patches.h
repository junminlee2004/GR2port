// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

// Which of the built-in Gravity Rush 2 patches apply. The launcher's settings decide for every
// patch, whatever the patch file's isEnabled flags say.
namespace Gr2Port::GamePatches {

struct FrameRate {
    u32 fps;
    /// The patch for this rate, empty for the game's own 30.
    std::string_view patch;
};

/// Each rate's patch runs with the vblank at that rate.
inline constexpr std::array FrameRates{
    FrameRate{30, ""},
    FrameRate{40, "40fps v6.0 test (READ NOTES)"},
    FrameRate{60, "60fps v6.0 test (READ NOTES)"},
    FrameRate{90, "90fps v6.0 test (READ NOTES)"},
    FrameRate{120, "120fps v6.0 test (READ NOTES)"},
    FrameRate{144, "144fps v6.0 test (READ NOTES)"},
    FrameRate{165, "165fps v6.0 test (READ NOTES)"},
    FrameRate{240, "240fps v6.0 test (READ NOTES)"},
    FrameRate{360, "360fps v6.0 test (READ NOTES)"},
};

struct Aspect {
    std::string_view name;
    /// The category of the aspect's resolution patches, one for each render size but the game's
    /// own 16:9 1080p.
    std::string_view category;
    float ratio;
};

inline constexpr std::array Aspects{
    Aspect{"16:9", "Resolution/No upscaling/16:9", 16.0f / 9.0f},
    Aspect{"16:10", "Resolution/No upscaling/16:10", 16.0f / 10.0f},
    Aspect{"21:10", "Resolution/No upscaling/Ultrawide 21:10", 21.0f / 10.0f},
    Aspect{"21:9", "Resolution/No upscaling/Ultrawide 21:9", 7.0f / 3.0f},
    Aspect{"2.39:1", "Resolution/No upscaling/Ultrawide 2.39:1", 2.39f},
    Aspect{"32:10", "Resolution/No upscaling/Super ultrawide 32:10", 32.0f / 10.0f},
    Aspect{"32:9", "Resolution/No upscaling/Super ultrawide 32:9", 32.0f / 9.0f},
};

struct RenderSize {
    std::string_view name;
    /// The height of the size's patches; 16:10's keep the 16:9 width and are 10/9 as tall.
    u32 lines;
};

inline constexpr std::array RenderSizes{
    RenderSize{"544p", 544},   RenderSize{"720p", 720},   RenderSize{"900p", 900},
    RenderSize{"1080p", 1080}, RenderSize{"1440p", 1440}, RenderSize{"4K", 2160},
    RenderSize{"5K", 2880},    RenderSize{"8K", 4320},
};

/// GR2 online is always on, with these servers, whatever the config files say.
inline constexpr std::string_view ShadNetServer = "srv.shadps4.net:31313";
inline constexpr std::string_view ShadNetWebApiServer = "http://srv.shadps4.net:31315";
inline constexpr int P2pPort = 3658;
/// The game's own server.
inline constexpr std::string_view GameServerHost = "45.76.253.42";
inline constexpr std::string_view ShadNetSignUp = "https://shadps4.net/shadnet/register/";
inline constexpr std::string_view ShadNetTrophies = "https://shadnet.shadps4.net/#/trophies";

inline constexpr std::string_view DlcPatch = "Unlock All DLC";
inline constexpr std::string_view LooseFilesPatch =
    "Boot with loose files exclusively (READ NOTES)";

struct Extra {
    std::string name;
    /// The patch file's own flag.
    bool listed_on;
};

/// The patches the Cheats & extras tab lists, in the patch file's order: all but the frame rate,
/// resolution, DLC and loose-files patches.
std::vector<Extra> Extras();

/// Whether the built-in patch named name, of category category, applies. listed_on is its flag in
/// the patch file.
bool IsEnabled(std::string_view name, std::string_view category, bool listed_on);

/// After the game's own settings load: sets the vblank to the chosen frame rate's, raises the
/// extra direct memory to what the chosen resolution's patch needs, turns GR2 online on, and
/// queues the patches that scale the game's level of detail distances and its areas' margins
/// to the chosen detail.
void ApplySettings();

/// The chosen aspect's width over height.
float AspectRatio();

/// Whether the Halloween photo items mod pack is on.
bool HalloweenItems();

} // namespace Gr2Port::GamePatches
