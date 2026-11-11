// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/logging/log.h"
#include "input/mouse_look.h"

#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/mouse_camera.h"
#include "graphics/gr2port/settings.h"

namespace Gr2Port::MouseCamera {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// bool FUN_0155be10(float weight, state, float4* orientation, const float4* frame, up, out): the
// orientation and frame are quaternions (x, y, z, w); it returns whether the stick turned it.
constexpr Hooks::Entry Turn{0x155be10, 6, Hooks::FramePrologue};

// FUN_01562700, the gravity shift camera, one of its callers.
constexpr VAddr ShiftCameraStart = 0x1562700;
constexpr VAddr ShiftCameraEnd = 0x1562700 + 15806;

// void FUN_01567bd0(camera, ...): the camera on foot turns its orientation at camera + 0xc0 by the
// stick itself, about the world's Y axis.
constexpr Hooks::Entry Follow{0x1567bd0, 6, Hooks::FramePrologue};
constexpr u64 FollowOrientation = 0xc0;

// The game's camera options: pitch and yaw inverted (bytes), and the camera sensitivity, slider
// value / 50, 1 by default.
constexpr VAddr PitchInverted = 0x1f2cac4;
constexpr VAddr YawInverted = 0x1f2cac5;
constexpr VAddr Sensitivity = 0x1f2cac8;

// The cameras read the right stick only while the stick lock is 0 and the active pad's input byte
// is set (FUN_0155be10, FUN_01567bd0). The pad index is the high half of the camera mode word;
// mode 1 makes FUN_01567bd0 set a new lock before it reads the stick.
constexpr VAddr StickLock = 0x1f2ef60;
constexpr VAddr CameraMode = 0x1f2f510;
constexpr VAddr ActivePad = 0x1f2f514;
constexpr VAddr Pads = 0x1eeec80;
constexpr u64 PadBytes = 0x900;
constexpr u64 PadInput = 0x710;

// Radians the camera turns per mouse count at the default sensitivity, about 0.023 degrees.
constexpr float RadiansPerCount = 0.0004f;

// The mouse pitches the camera at most this far from the frame's horizon, about 85 degrees, as the
// stick stops short of looking straight up or down. Past the pole the camera's up points down and
// a yaw about the frame's up turns it against the mouse.
constexpr float MaxPitch = 1.48f;

struct Quaternion {
    float x;
    float y;
    float z;
    float w;
};

constexpr Quaternion Identity{0.0f, 0.0f, 0.0f, 1.0f};

using TurnFunction = bool PS4_SYSV_ABI (*)(float weight, u64 state, u64 orientation, u64 frame,
                                           u64 up, u64 out);

// Horizontal and vertical speed factors from the launcher's Mouse && Keyboard tab.
struct Speeds {
    float horizontal;
    float vertical;
};

VAddr g_base = 0;
TurnFunction g_turn = nullptr;
Speeds g_foot{1.0f, 1.0f};
Speeds g_shift{1.0f, 1.0f};

Quaternion Multiply(const Quaternion& a, const Quaternion& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quaternion Conjugate(const Quaternion& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

Quaternion AboutAxis(float x, float y, float z, float angle) {
    const float half = angle * 0.5f;
    const float s = std::sin(half);
    return {x * s, y * s, z * s, std::cos(half)};
}

template <typename T>
T Read(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(g_base + address - GoldHenBase),
                sizeof(value));
    return value;
}

// Whether the game's cameras take the right stick now.
bool StickTurnsCamera() {
    const VAddr pads = Read<VAddr>(Pads);
    if (Read<s32>(StickLock) != 0 || pads == 0) {
        return false;
    }
    const VAddr pad = pads + u64{Read<u32>(ActivePad)} * PadBytes;
    return *reinterpret_cast<const u8*>(pad + PadInput) != 0;
}

// Turns the orientation quaternion at address by the mouse's movement since the last call, in the
// space of frame: yaw before the orientation, pitch after it. Returns whether the mouse moved.
// While the game ignores the stick, the movement is dropped, as the stick's would be.
bool TurnByMouse(VAddr orientation, const Quaternion& frame, const Speeds& speeds, bool takes) {
    const auto [dx, dy] = Input::MouseLook::Take();
    if (!takes || (dx == 0.0f && dy == 0.0f)) {
        return false;
    }
    const float scale = RadiansPerCount * Read<float>(Sensitivity);
    float yaw = -dx * scale * speeds.horizontal * (Read<u8>(YawInverted) != 0 ? -1.0f : 1.0f);
    float pitch = -dy * scale * speeds.vertical * (Read<u8>(PitchInverted) != 0 ? -1.0f : 1.0f);

    Quaternion q;
    std::memcpy(&q, reinterpret_cast<const void*>(orientation), sizeof(q));
    Quaternion local = Multiply(Conjugate(frame), q);

    // The frame's Y components of the camera's Y and Z axes give its pitch, in the sense the pitch
    // below turns it. A camera the game left upside down yaws the other way, so the view still
    // follows the mouse.
    const float up_y = 1.0f - 2.0f * (local.x * local.x + local.z * local.z);
    const float z_y = 2.0f * (local.y * local.z - local.x * local.w);
    const float current = std::atan2(-z_y, up_y);
    if (pitch > 0.0f) {
        pitch = std::min(pitch, std::max(0.0f, MaxPitch - current));
    } else {
        pitch = std::max(pitch, std::min(0.0f, -MaxPitch - current));
    }
    if (up_y < 0.0f) {
        yaw = -yaw;
    }
    local = Multiply(AboutAxis(0.0f, 1.0f, 0.0f, yaw), local);
    local = Multiply(local, AboutAxis(1.0f, 0.0f, 0.0f, pitch));
    Quaternion result = Multiply(frame, local);
    const float length = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z +
                                   result.w * result.w);
    if (!(length > 0.0f)) {
        return false;
    }
    result = {result.x / length, result.y / length, result.z / length, result.w / length};
    std::memcpy(reinterpret_cast<void*>(orientation), &result, sizeof(result));
    return true;
}

bool PS4_SYSV_ABI OnTurn(float weight, u64 state, u64 orientation, u64 frame, u64 up, u64 out) {
    const VAddr caller = reinterpret_cast<VAddr>(__builtin_return_address(0));
    const bool shifting = caller >= g_base + ShiftCameraStart - GoldHenBase &&
                          caller < g_base + ShiftCameraEnd - GoldHenBase;
    const bool turned = g_turn(weight, state, orientation, frame, up, out);
    Quaternion base;
    std::memcpy(&base, reinterpret_cast<const void*>(frame), sizeof(base));
    return TurnByMouse(orientation, base, shifting ? g_shift : g_foot, StickTurnsCamera()) ||
           turned;
}

void PS4_SYSV_ABI OnFollow(u32, const Hooks::Registers* registers, u64) {
    const bool takes = static_cast<u32>(Read<u64>(CameraMode)) != 1 && StickTurnsCamera();
    TurnByMouse(registers->rdi + FollowOrientation, Identity, g_foot, takes);
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    const Settings::Mouse mouse = Settings::Get().mouse;
    g_foot = {mouse.foot_horizontal / 100.0f, mouse.foot_vertical / 100.0f};
    g_shift = {mouse.shift_horizontal / 100.0f, mouse.shift_vertical / 100.0f};
    const void* original = nullptr;
    if (Hooks::Replace(module_base, Turn, c, capacity, reinterpret_cast<const void*>(&OnTurn),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera turn is not the expected code");
        return false;
    }
    g_turn = reinterpret_cast<TurnFunction>(const_cast<void*>(original));
    const Hooks::Result follow =
        Hooks::ObserveEntry(module_base, Follow, c, capacity, &OnFollow, 0);
    if (follow != Hooks::Result::Hooked && follow != Hooks::Result::Chained) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera on foot is not the expected code");
    }
    LOG_INFO(Render_Vulkan, "GR2port: the mouse turns the camera");
    return true;
}

} // namespace Gr2Port::MouseCamera
