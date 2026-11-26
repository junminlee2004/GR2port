// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/recompiled/recompiled.h"
#include "input/controller.h"

#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/stick_tilt.h"

namespace Gr2Port::StickTilt {

namespace {

// void FUN_01397200(input, bool reset): a menu input's tilt. The reference quaternion R at
// input + 0x120, which a reset sets to the inverse of the pad's orientation Q, gives the tilt
// R * Q. Its X and -Z components, each held within the sine of half the angle at input + 0x184
// (in degrees), are what the menu turns by.
constexpr Hooks::Entry Tilt{0x1397200, 6, Hooks::FramePrologueR14};
constexpr u64 Reference = 0x120;
constexpr u64 MaxDegrees = 0x184;

// FUN_01396ec0 copies the right stick into the input: x right, y up, each -1 to 1.
constexpr u64 RightStick = 0x100;

// bool FUN_017772a0(page): a comic page's update, with a tilt of its own. A reset sets the
// reference quaternion R at page + 0xb0 to the inverse of the pad's orientation Q, and the page
// turns by the X and -Z components of R * Q, each times 0.1 and held within sin(0.0872664675).
constexpr Hooks::Entry ComicPage{0x17772a0, 6, Hooks::FramePrologue};
constexpr u64 ComicReference = 0xb0;
constexpr float ComicScale = 0.1f;
constexpr float ComicLimitAngle = 0.0872664675f;

// A comic page whose update ran this long ago still shows.
using Clock = std::chrono::steady_clock;
constexpr auto ComicPageShowing = std::chrono::milliseconds(200);

// FUN_01396ec0 reads the pad's state with the pad manager's vtable + 0x28, the user and whether
// the user's byte 4 is set, and copies the right stick from the state + 0x74.
constexpr VAddr PadManager = 0x1f1d860;
constexpr VAddr PadUser = 0x1f1c340;
constexpr u64 PadState = 0x28;
constexpr u64 StateRightStick = 0x74;

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

using TiltFunction = u64 PS4_SYSV_ABI (*)(u64 input, u64 reset);
using ComicPageFunction = u64 PS4_SYSV_ABI (*)(u64 page);
using PadStateFunction = u64 PS4_SYSV_ABI (*)(u64 manager, u64 user, u64 user_byte_4);

VAddr g_module_base = 0;
TiltFunction g_tilt = nullptr;
ComicPageFunction g_comic_page = nullptr;

// When a comic page last ran its update, in Clock ticks; 0 before the first.
std::atomic<Clock::rep> g_comic_page_time{0};

Quaternion Multiply(const Quaternion& a, const Quaternion& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

template <typename T>
T Load(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

template <typename T>
void Store(u64 address, const T& value) {
    std::memcpy(reinterpret_cast<void*>(address), &value, sizeof(value));
}

bool PadHasGyro() {
    return (*Common::Singleton<Input::GameControllers>::Instance())[0]->HasGyro();
}

// The stick's turn, each component at most amount. Up raises the pad's top and right lowers its
// right side, as a gyro reports them.
Quaternion Turn(const std::array<float, 2>& stick, float amount) {
    const float x = stick[1] * amount;
    const float z = -stick[0] * amount;
    return {x, 0.0f, z, std::sqrt(std::max(0.0f, 1.0f - x * x - z * z))};
}

// The right stick of the pad's state that menus copy: x right, y up, each -1 to 1.
std::array<float, 2> PadRightStick() {
    const u64 manager = Load<u64>(g_module_base + PadManager - Hooks::CodeBase);
    if (manager == 0) {
        return {};
    }
    const u64 user = Load<u64>(g_module_base + PadUser - Hooks::CodeBase);
    const auto state_of =
        reinterpret_cast<PadStateFunction>(Load<u64>(Load<u64>(manager) + PadState));
    const u64 state = state_of(manager, user, (user & 0xff00000000ULL) != 0 ? 1 : 0);
    return Load<std::array<float, 2>>(state + StateRightStick);
}

// While the original runs, the reference also turns by the stick, so the tilt is the stick's
// turn after the pad's own. A reset keeps the game's reference.
u64 PS4_SYSV_ABI OnTilt(u64 input, u64 reset) {
    const auto stick = Load<std::array<float, 2>>(input + RightStick);
    if (static_cast<u8>(reset) != 0 || (stick[0] == 0.0f && stick[1] == 0.0f) || PadHasGyro()) {
        return Core::Recompiled::Call(g_tilt, input, reset);
    }
    const float limit =
        std::sin(Load<float>(input + MaxDegrees) * (std::numbers::pi_v<float> / 360.0f));
    const auto reference = Load<Quaternion>(input + Reference);
    Store(input + Reference, Multiply(Turn(stick, limit), reference));
    const u64 result = Core::Recompiled::Call(g_tilt, input, reset);
    Store(input + Reference, reference);
    return result;
}

// The comic page's reference turns by the stick while its update runs, as the menu's does, so
// full deflection reaches the page's limit. A reference the update sets itself stays.
u64 PS4_SYSV_ABI OnComicPage(u64 page) {
    g_comic_page_time.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    const auto stick = PadRightStick();
    if ((stick[0] == 0.0f && stick[1] == 0.0f) || PadHasGyro()) {
        return Core::Recompiled::Call(g_comic_page, page);
    }
    const float amount = std::sin(ComicLimitAngle) / ComicScale;
    const auto reference = Load<Quaternion>(page + ComicReference);
    const Quaternion turned = Multiply(Turn(stick, amount), reference);
    Store(page + ComicReference, turned);
    const u64 result = Core::Recompiled::Call(g_comic_page, page);
    const auto after = Load<Quaternion>(page + ComicReference);
    if (std::memcmp(&after, &turned, sizeof(turned)) == 0) {
        Store(page + ComicReference, reference);
    }
    return result;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_module_base = module_base;
    const void* original = nullptr;
    if (Hooks::Replace(module_base, Tilt, c, capacity, reinterpret_cast<const void*>(&OnTilt),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the menu tilt is not the expected code");
        return false;
    }
    g_tilt = reinterpret_cast<TiltFunction>(const_cast<void*>(original));
    if (Hooks::Replace(module_base, ComicPage, c, capacity,
                       reinterpret_cast<const void*>(&OnComicPage),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the comic page's tilt is not the expected code");
        return false;
    }
    g_comic_page = reinterpret_cast<ComicPageFunction>(const_cast<void*>(original));
    return true;
}

bool ComicPageShown() {
    const Clock::rep last = g_comic_page_time.load(std::memory_order_relaxed);
    return last != 0 && Clock::now() - Clock::time_point{Clock::duration{last}} <= ComicPageShowing;
}

} // namespace Gr2Port::StickTilt
