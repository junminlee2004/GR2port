// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/photo_tilt.h"
#include "graphics/gr2port/photo_ui.h"
#include "input/mouse_look.h"

namespace Gr2Port::PhotoTilt {

namespace {

// FUN_015edda0, which blends the cameras into the camera manager's view, r15: call FUN_014cec20
// builds the view's axes from its eye, the point it looks at and its up vector, which the
// function wrote just before. The manager then keeps the view as the pose the next frame's blend
// starts from, where the camera in use writes its own.
constexpr Hooks::Entry FinishView{0x15eecfd, 5, {0xe8, 0x1e, 0xff, 0xed, 0xff}};
constexpr VAddr FinishPose = 0x14cec20;
constexpr u64 ViewPose = 0x10;

// The quaternion of the frame that follows Kat's gravity: its Y axis is her up.
constexpr VAddr GravityFrame = 0x1f2f340;

// The stub, its jump from the site, and the room the jump's Replace asks for.
constexpr size_t StubBytes = 64 + 14 + Hooks::MaxStubBytes;

// Radians the camera turns in a second at the stick's full tilt, the share of the stick's travel
// from its middle that turns nothing, and the longest frame a turn counts.
constexpr float Speed = 1.5f;
constexpr float DeadZone = 0.2f;
// Radians the camera turns for each count the mouse moves.
constexpr float RadiansPerCount = 0.001f;
constexpr float LongestFrame = 0.1f;
constexpr float HalfTurn = std::numbers::pi_v<float>;
constexpr float QuarterTurn = 0.5f * HalfTurn;

struct Vector {
    float x;
    float y;
    float z;
};

Vector operator+(const Vector& a, const Vector& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vector operator-(const Vector& a, const Vector& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vector operator*(const Vector& a, float scale) {
    return {a.x * scale, a.y * scale, a.z * scale};
}

float Dot(const Vector& a, const Vector& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector Cross(const Vector& a, const Vector& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// A pose's eye, the point it looks at and its up vector, as the game holds them.
struct Pose {
    Vector eye;
    float eye_w;
    Vector focus;
    float focus_w;
    Vector up;
    float up_w;
};
static_assert(sizeof(Pose) == 0x30);

VAddr g_base = 0;
std::atomic<float> g_stick_x{0.0f};
std::atomic<float> g_stick_y{0.0f};
std::atomic<bool> g_key{false};
// The game's main thread: the camera's roll and tilt in radians, when the view last took them,
// and the pose the game gave then with the pose written over it.
float g_roll = 0.0f;
float g_tilt = 0.0f;
std::chrono::steady_clock::time_point g_turned;
Pose g_given{};
Pose g_written{};
bool g_wrote = false;

// The stick's tilt past the dead zone, from -1 to 1.
float Travel(float tilt) {
    const float past = std::max(0.0f, std::abs(tilt) - DeadZone) / (1.0f - DeadZone);
    return std::copysign(std::min(past, 1.0f), tilt);
}

// Kat's up: the Y axis of the gravity frame's quaternion.
Vector Sky() {
    float q[4];
    std::memcpy(q, Hooks::Code(g_base, GravityFrame), sizeof(q));
    return {2.0f * (q[0] * q[1] - q[2] * q[3]), 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]),
            2.0f * (q[1] * q[2] + q[0] * q[3])};
}

// Before the manager builds its view's axes: turns the view by the camera's roll and tilt.
void PS4_SYSV_ABI TurnView(VAddr view) {
    const auto now = std::chrono::steady_clock::now();
    const float seconds =
        std::min(std::chrono::duration<float>(now - g_turned).count(), LongestFrame);
    g_turned = now;
    if (!PhotoUi::CameraOut()) {
        g_roll = 0.0f;
        g_tilt = 0.0f;
        g_wrote = false;
        return;
    }
    Pose pose;
    std::memcpy(&pose, reinterpret_cast<const void*>(view + ViewPose), sizeof(pose));
    // A frame without a camera's own pose blends the view written here.
    if (g_wrote && std::memcmp(&pose, &g_written, sizeof(pose)) == 0) {
        pose = g_given;
    }
    g_given = pose;
    g_wrote = false;

    const Vector sight = pose.focus - pose.eye;
    const float distance = std::sqrt(Dot(sight, sight));
    if (!(distance > 0.0f)) {
        return;
    }
    const Vector forward = sight * (1.0f / distance);
    const Vector over = pose.up - forward * Dot(pose.up, forward);
    const float over_length = std::sqrt(Dot(over, over));
    if (!(over_length > 0.0f)) {
        return;
    }
    const Vector up = over * (1.0f / over_length);
    const Vector right = Cross(forward, up);

    // The tilt ends where the camera looks straight down or straight up.
    const float pitch = std::asin(std::clamp(Dot(forward, Sky()), -1.0f, 1.0f));
    float roll = Travel(g_stick_x.load(std::memory_order_relaxed)) * Speed * seconds;
    float tilt = Travel(g_stick_y.load(std::memory_order_relaxed)) * Speed * seconds;
    if (g_key.load(std::memory_order_relaxed)) {
        const auto [dx, dy] = Input::MouseLook::Take();
        roll += dx * RadiansPerCount;
        tilt += dy * RadiansPerCount;
    }
    g_roll = std::remainder(g_roll + roll, 2.0f * HalfTurn);
    g_tilt = std::clamp(g_tilt + tilt, -QuarterTurn - pitch, QuarterTurn - pitch);
    if (g_roll == 0.0f && g_tilt == 0.0f) {
        return;
    }
    const Vector ahead = forward * std::cos(g_tilt) + up * std::sin(g_tilt);
    const Vector above = up * std::cos(g_tilt) - forward * std::sin(g_tilt);
    pose.focus = pose.eye + ahead * distance;
    pose.up = above * std::cos(g_roll) + right * std::sin(g_roll);
    std::memcpy(reinterpret_cast<void*>(view + ViewPose), &pose, sizeof(pose));
    g_written = pose;
    g_wrote = true;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (!Hooks::Holds(module_base, FinishView)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera manager's view is not the expected code");
        return false;
    }
    if (capacity - c.getSize() < StubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the hook of the camera manager's view");
        return false;
    }
    g_base = module_base;
    const void* const stub = c.getCurr();
    c.mov(rdi, r15);
    c.mov(rax, reinterpret_cast<u64>(&TurnView));
    c.call(rax);
    c.mov(rdi, r15);
    c.call(Hooks::Code(module_base, FinishPose));
    c.jmp(Hooks::Code(module_base, FinishView.address + FinishView.size));
    if (Hooks::Replace(module_base, FinishView, c, capacity, stub) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera manager's view could not be hooked");
        return false;
    }
    return true;
}

void SetStick(float x, float y) {
    g_stick_x.store(x, std::memory_order_relaxed);
    g_stick_y.store(y, std::memory_order_relaxed);
}

void SetKey(bool held) {
    g_key.store(held, std::memory_order_relaxed);
}

bool TakesMouse() {
    return g_key.load(std::memory_order_relaxed) && PhotoUi::CameraOut();
}

} // namespace Gr2Port::PhotoTilt
