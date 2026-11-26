// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "input/mouse_look.h"

#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/mouse_camera.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/stick_tilt.h"

namespace Gr2Port::MouseCamera {

namespace {

// bool FUN_0155be10(float weight, state, float4* orientation, const float4* frame, up, out): the
// orientation and frame are quaternions (x, y, z, w); it returns whether the stick turned it.
constexpr Hooks::Entry Turn{0x155be10, 6, Hooks::FramePrologue};

// FUN_01562700, the gravity shift camera, one of its callers.
constexpr VAddr ShiftCameraStart = 0x1562700;
constexpr VAddr ShiftCameraEnd = 0x1562700 + 15806;

// void FUN_01567bd0(camera, ...): the camera on foot turns its orientation at camera + 0xc0 by the
// stick itself, about the Y axis of the quaternion at FollowFrame, which follows Kat's gravity: on
// a wall its Y axis is the wall's normal.
constexpr Hooks::Entry Follow{0x1567bd0, 6, Hooks::FramePrologue};
constexpr u64 FollowOrientation = 0xc0;
// While the follow camera eases its pitch, as after a change of gravity style, it rebuilds the
// orientation from the copy at +0xb0, which drops a turn written at FollowOrientation. This float
// is the time the easing lasts, and the game zeroes it to end the easing when the stick turns the
// pitch.
constexpr u64 FollowEaseTime = 0x1cc;
constexpr VAddr FollowFrame = 0x1f2f340;

// The game's camera options: yaw and pitch inverted (bytes), and the camera sensitivity, slider
// value / 50, 1 by default.
constexpr VAddr YawInverted = 0x1f2cac4;
constexpr VAddr PitchInverted = 0x1f2cac5;
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

// The camera manager: FUN_015d7100 blends the view from the pose at BlendStart to the active
// camera's pose while BlendLeft counts down. A change of camera, as for a gravity shift, starts a
// blend. A pose holds the eye, the point looked at and the up vector.
constexpr VAddr CameraManager = 0x1f30620;
constexpr u64 BlendStart = 0x100;
constexpr u64 BlendLeft = 0x2b4;
constexpr u64 PoseEye = 0x10;
constexpr u64 PoseFocus = 0x20;
constexpr u64 PoseUp = 0x30;

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

struct Vector {
    float x;
    float y;
    float z;
};

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

Vector Cross(const Vector& a, const Vector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Dot(const Vector& a, const Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// The vector turned by a unit quaternion.
Vector Turned(const Quaternion& q, const Vector& v) {
    const Vector axis{q.x, q.y, q.z};
    const Vector c = Cross(axis, v);
    const Vector d = Cross(axis, {c.x + q.w * v.x, c.y + q.w * v.y, c.z + q.w * v.z});
    return {v.x + 2.0f * d.x, v.y + 2.0f * d.y, v.z + 2.0f * d.z};
}

Vector LoadVector(VAddr address) {
    Vector v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v;
}

void StoreVector(VAddr address, const Vector& v) {
    std::memcpy(reinterpret_cast<void*>(address), &v, sizeof(v));
}

template <typename T>
T Read(VAddr address) {
    T value;
    std::memcpy(&value, Hooks::Code(g_base, address), sizeof(value));
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

// The part of pitch that keeps a camera within MaxPitch of its frame's horizon, given the frame's Y
// components of the camera's forward and up axes.
float LimitPitch(float pitch, float forward_y, float up_y) {
    const float current = std::atan2(forward_y, up_y);
    if (pitch > 0.0f) {
        return std::min(pitch, std::max(0.0f, MaxPitch - current));
    }
    return std::max(pitch, std::min(0.0f, -MaxPitch - current));
}

// During a blend the view keeps near the pose the blend starts from, so a turn of the active camera
// alone shows only as the blend ends. The start pose takes the same yaw about frame_up and the same
// pitch about its own right, around the point it looks at. It can face another way than the active
// camera, whose right would pitch it against the mouse.
void TurnBlendStart(const Vector& frame_up, float yaw, float pitch) {
    const VAddr manager = Read<VAddr>(CameraManager);
    if (manager == 0 || *reinterpret_cast<const u32*>(manager + BlendLeft) == 0) {
        return;
    }
    const VAddr start = manager + BlendStart;
    const Vector eye = LoadVector(start + PoseEye);
    const Vector focus = LoadVector(start + PoseFocus);
    const Vector up = LoadVector(start + PoseUp);
    const Vector offset{eye.x - focus.x, eye.y - focus.y, eye.z - focus.z};
    const Vector side = Cross(up, offset);
    const float distance = std::sqrt(Dot(offset, offset));
    const float side_length = std::sqrt(Dot(side, side));
    if (!(distance > 0.0f) || !(side_length > 0.0f)) {
        return;
    }
    const Vector back{offset.x / distance, offset.y / distance, offset.z / distance};
    const Vector right{side.x / side_length, side.y / side_length, side.z / side_length};
    const float up_y = Dot(Cross(back, right), frame_up);
    const Quaternion turn = Multiply(
        AboutAxis(frame_up.x, frame_up.y, frame_up.z, up_y < 0.0f ? -yaw : yaw),
        AboutAxis(right.x, right.y, right.z, LimitPitch(pitch, -Dot(back, frame_up), up_y)));
    const Vector turned = Turned(turn, offset);
    StoreVector(start + PoseEye, {focus.x + turned.x, focus.y + turned.y, focus.z + turned.z});
    StoreVector(start + PoseUp, Turned(turn, up));
}

// Turns the orientation quaternion at address by the mouse's movement since the last call, in the
// space of frame: yaw before the orientation, pitch after it. Returns whether the mouse moved.
// While the game ignores the stick, the movement is dropped, as the stick's would be. While a comic
// page shows, the camera leaves the mouse to the right stick, which tilts the page.
bool TurnByMouse(VAddr orientation, const Quaternion& frame, const Speeds& speeds, bool takes) {
    if (StickTilt::ComicPageShown()) {
        return false;
    }
    const auto [dx, dy] = Input::MouseLook::Take();
    if (!takes || (dx == 0.0f && dy == 0.0f)) {
        return false;
    }
    const float scale = RadiansPerCount * Read<float>(Sensitivity);
    const float yaw = -dx * scale * speeds.horizontal * (Read<u8>(YawInverted) != 0 ? -1.0f : 1.0f);
    const float pitch =
        -dy * scale * speeds.vertical * (Read<u8>(PitchInverted) != 0 ? -1.0f : 1.0f);

    Quaternion q;
    std::memcpy(&q, reinterpret_cast<const void*>(orientation), sizeof(q));
    Quaternion local = Multiply(Conjugate(frame), q);

    // The frame's Y components of the camera's Y and Z axes give its pitch, in the sense the pitch
    // below turns it. A camera the game left upside down yaws the other way, so the view still
    // follows the mouse.
    const float up_y = 1.0f - 2.0f * (local.x * local.x + local.z * local.z);
    const float z_y = 2.0f * (local.y * local.z - local.x * local.w);
    local = Multiply(AboutAxis(0.0f, 1.0f, 0.0f, up_y < 0.0f ? -yaw : yaw), local);
    local = Multiply(local, AboutAxis(1.0f, 0.0f, 0.0f, LimitPitch(pitch, -z_y, up_y)));
    Quaternion result = Multiply(frame, local);
    const float length = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z +
                                   result.w * result.w);
    if (!(length > 0.0f)) {
        return false;
    }
    result = {result.x / length, result.y / length, result.z / length, result.w / length};
    std::memcpy(reinterpret_cast<void*>(orientation), &result, sizeof(result));
    TurnBlendStart(Turned(frame, {0.0f, 1.0f, 0.0f}), yaw, pitch);
    return true;
}

bool PS4_SYSV_ABI OnTurn(float weight, u64 state, u64 orientation, u64 frame, u64 up, u64 out) {
    const VAddr caller = Core::Recompiled::GuestCaller(__builtin_return_address(0));
    const bool shifting = caller >= g_base + ShiftCameraStart - Hooks::CodeBase &&
                          caller < g_base + ShiftCameraEnd - Hooks::CodeBase;
    const bool turned = Core::Recompiled::Call(g_turn, weight, state, orientation, frame, up, out);
    // The stick turns the gravity shift camera about its own axes at any pitch, so the mouse turns
    // it in its own frame.
    Quaternion base;
    std::memcpy(&base, reinterpret_cast<const void*>(shifting ? orientation : frame), sizeof(base));
    return TurnByMouse(orientation, base, shifting ? g_shift : g_foot, StickTurnsCamera()) ||
           turned;
}

void PS4_SYSV_ABI OnFollow(u32, const Hooks::Registers* registers, u64) {
    const bool takes = static_cast<u32>(Read<u64>(CameraMode)) != 1 && StickTurnsCamera();
    if (TurnByMouse(registers->rdi + FollowOrientation, Read<Quaternion>(FollowFrame), g_foot,
                    takes)) {
        *reinterpret_cast<float*>(registers->rdi + FollowEaseTime) = 0.0f;
    }
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
    if (!Hooks::Hooked(follow)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera on foot is not the expected code");
    }
    LOG_INFO(Render_Vulkan, "GR2port: the mouse turns the camera");
    return true;
}

} // namespace Gr2Port::MouseCamera
