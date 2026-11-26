// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/script_timers.h"
#include "graphics/gr2port/scripted_moves.h"

namespace Gr2Port::ScriptedMoves {

namespace {

// FUN_016ca930, the script function setWorldPos, with the object in rdi and its new position
// stored: call FUN_0160d0d0 derives the object's local position from it.
constexpr Hooks::Entry ScriptMoveCall{0x16caa21, 5, {0xe8, 0xaa, 0x26, 0xf4, 0xff}};
constexpr VAddr ScriptMoveNext = 0x16caa26;
constexpr VAddr LocalFromWorld = 0x160d0d0;

// FUN_015d7100, a frame of the game, after its last group of scripts and before its camera:
// movzx eax, byte [AfterScriptsByte]. One way here comes out of a call, so the code after it
// relies on no caller-saved register.
constexpr Hooks::Entry AfterScripts{0x15d80dd, 7, {0x0f, 0xb6, 0x05, 0x3d, 0x78, 0x70, 0x00}};
constexpr VAddr AfterScriptsByte = 0x1cdf921;
constexpr VAddr AfterScriptsNext = 0x15d80e4;

// FUN_015d7100's call of FUN_015edda0, which aims the camera. No register survives into the code
// after it.
constexpr Hooks::Entry CameraCall{0x15d9030, 5, {0xe8, 0x6b, 0x4d, 0x01, 0x00}};
constexpr VAddr CameraFunction = 0x15edda0;
constexpr VAddr CameraCallNext = 0x15d9035;

// The game clock's step in seconds. At zero the game puts a body straight at its object's position.
constexpr VAddr StepTime = 0x1efb104;

// The frame rate patches' length of this frame in 30 fps frames, at most 1.
constexpr VAddr FrameShare = 0x1f7c600;

// An object's world position, its count of moves, and its flags with the bit for a changed
// transform.
constexpr u64 WorldPosition = 0x2c0;
constexpr u64 MoveCount = 0x320;
constexpr u64 TransformFlags = 0x328;
constexpr u8 TransformChanged = 1;

// In an object with a physics body: the count of moves its body has taken, and the body's position.
constexpr u64 BodyMoveCount = 0x37c;
constexpr u64 BodyPosition = 0x3c0;

// In such an object, the velocity the next sync adds to the body's when the flag is set. The game
// stores it once per 30 fps frame, so above 30 fps only some frames would land short of the path.
constexpr u64 BodyFlags = 0x380;
constexpr u8 BodyHasExtraVelocity = 4;
constexpr u64 BodyExtraVelocity = 0x390;

// How far the physics moves a body toward its object in a 30 fps frame: 100 units a second.
constexpr float BodyReach = 100.0f / 30.0f;

// How long a script's position holds, in 30 fps frames. A script that moves an object every 30 fps
// frame comes back within this time when a frame runs long.
constexpr float HeldFrames = 1.5f;

// How long an object takes to reach a script's position, in 30 fps frames. The script's next
// position comes a few milliseconds before or after one 30 fps frame, and the object keeps moving
// through the difference.
constexpr float EaseFrames = 1.25f;

// The three stubs.
constexpr size_t StubBytes = 96;

using Position = std::array<float, 4>;

struct Move {
    VAddr object;
    // The object's vtable, which reads differently once the object is destroyed.
    VAddr type;
    // Where the object eases from, the script's latest position, and the object's last position.
    Position from;
    Position to;
    Position shown;
    // Where the body is headed at its 30 fps speed. Gravity takes a little off each short step, so
    // the body's own position would fall behind this.
    Position body;
    // The time since the script's latest position, in 30 fps frames.
    float waited;
    // Whether something else moves the object away from the script's position.
    bool pulled;
};

using LocalFromWorldFunction = void PS4_SYSV_ABI (*)(VAddr object);

LocalFromWorldFunction g_local_from_world = nullptr;
const float* g_step_time = nullptr;
const float* g_frame_share = nullptr;
std::array<Move, 32> g_moves{};
u32 g_moving = 0;

Position ReadPosition(VAddr address) {
    Position position;
    std::memcpy(position.data(), reinterpret_cast<const void*>(address), sizeof(position));
    return position;
}

// What setWorldPos does with a position.
void SetWorldPosition(VAddr object, const Position& position) {
    std::memcpy(reinterpret_cast<void*>(object + WorldPosition), position.data(), sizeof(position));
    *reinterpret_cast<u8*>(object + TransformFlags) |= TransformChanged;
    Core::Recompiled::Call(g_local_from_world, object);
    (*reinterpret_cast<u32*>(object + MoveCount))++;
}

bool IsGone(const Move& move) {
    return *reinterpret_cast<const VAddr*>(move.object) != move.type;
}

void PS4_SYSV_ABI OnScriptMove(VAddr object) {
    Core::Recompiled::Call(g_local_from_world, object);
    const Position position = ReadPosition(object + WorldPosition);
    Move* unused = nullptr;
    for (Move& move : g_moves) {
        if (move.object == object) {
            if (!move.pulled) {
                move.from = position;
                move.shown = position;
            } else if (move.waited > 0.0f) {
                move.from = move.shown;
            }
            move.to = position;
            move.waited = 0.0f;
            return;
        }
        if (move.object == 0 && unused == nullptr) {
            unused = &move;
        }
    }
    if (unused != nullptr) {
        const VAddr type = *reinterpret_cast<const VAddr*>(object);
        *unused = {object, type, position, position, position, position, 0.0f, false};
        g_moving++;
    }
}

void PS4_SYSV_ABI OnAfterScripts() {
    ScriptTimers::PlaceCages();
    if (g_moving == 0 || *g_step_time == 0.0f) {
        return;
    }
    const float frame = *g_frame_share;
    for (Move& move : g_moves) {
        if (move.object == 0) {
            continue;
        }
        move.waited += frame;
        if (move.waited >= HeldFrames || IsGone(move)) {
            move.object = 0;
            g_moving--;
            continue;
        }
        if (!move.pulled) {
            const Position position = ReadPosition(move.object + WorldPosition);
            const u32 moves = *reinterpret_cast<const u32*>(move.object + MoveCount);
            const u32 body_moves = *reinterpret_cast<const u32*>(move.object + BodyMoveCount);
            move.pulled = moves == body_moves &&
                          std::memcmp(position.data(), move.shown.data(), 3 * sizeof(float)) != 0;
            if (!move.pulled) {
                continue;
            }
            move.body = ReadPosition(move.object + BodyPosition);
        }
        const float share = std::min(move.waited / EaseFrames, 1.0f);
        for (size_t i = 0; i < move.shown.size(); i++) {
            move.shown[i] = move.from[i] + (move.to[i] - move.from[i]) * share;
        }
        SetWorldPosition(move.object, move.shown);
    }
}

// The camera has read the script's position. The body goes toward it no faster than at 30 fps.
void PS4_SYSV_ABI OnAfterCamera() {
    if (g_moving == 0 || *g_step_time == 0.0f) {
        return;
    }
    const float reach = BodyReach * *g_frame_share;
    for (Move& move : g_moves) {
        if (move.object == 0 || !move.pulled || IsGone(move)) {
            continue;
        }
        if (*reinterpret_cast<const u8*>(move.object + BodyFlags) & BodyHasExtraVelocity) {
            std::memset(reinterpret_cast<void*>(move.object + BodyExtraVelocity), 0,
                        sizeof(Position));
        }
        const float x = move.shown[0] - move.body[0];
        const float y = move.shown[1] - move.body[1];
        const float z = move.shown[2] - move.body[2];
        const float distance = std::sqrt(x * x + y * y + z * z);
        if (distance <= reach) {
            move.body = move.shown;
            continue;
        }
        const float share = reach / distance;
        move.body[0] += x * share;
        move.body[1] += y * share;
        move.body[2] += z * share;
        move.body[3] = move.shown[3];
        SetWorldPosition(move.object, move.body);
    }
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (!Hooks::Holds(module_base, ScriptMoveCall) || !Hooks::Holds(module_base, AfterScripts) ||
        !Hooks::Holds(module_base, CameraCall)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the scripted moves' code is not the expected code");
        return false;
    }
    if (capacity - c.getSize() < StubBytes + 3 * Hooks::MaxStubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the scripted moves' hooks");
        return false;
    }
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };
    g_local_from_world =
        reinterpret_cast<LocalFromWorldFunction>(reinterpret_cast<VAddr>(code(LocalFromWorld)));
    g_step_time = reinterpret_cast<const float*>(code(StepTime));
    g_frame_share = reinterpret_cast<const float*>(code(FrameShare));

    const void* script_move = c.getCurr();
    c.mov(rax, reinterpret_cast<u64>(&OnScriptMove));
    c.call(rax);
    c.jmp(code(ScriptMoveNext));

    const void* after_scripts = c.getCurr();
    c.mov(rax, reinterpret_cast<u64>(&OnAfterScripts));
    c.call(rax);
    c.movzx(eax, byte[rip + code(AfterScriptsByte)]);
    c.jmp(code(AfterScriptsNext));

    const void* camera = c.getCurr();
    c.call(code(CameraFunction));
    c.mov(rax, reinterpret_cast<u64>(&OnAfterCamera));
    c.call(rax);
    c.jmp(code(CameraCallNext));

    if (Hooks::Replace(module_base, ScriptMoveCall, c, capacity, script_move) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, AfterScripts, c, capacity, after_scripts) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, CameraCall, c, capacity, camera) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the scripted moves' hooks did not go in");
        return false;
    }
    return true;
}

} // namespace Gr2Port::ScriptedMoves
