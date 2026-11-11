// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/timeline.h"

namespace Gr2Port::Timeline {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The timeline player, FUN_010f7320(node).
constexpr Hooks::Entry Player{0x10f7320, 6, Hooks::FramePrologue};

// A node's whole-frame count, its playing flag, and the range it plays: start <= count < end.
constexpr u64 NodeStart = 0x3a0;
constexpr u64 NodeEnd = 0x3a4;
constexpr u64 NodeCount = 0x3b4;
constexpr u64 NodePlaying = 0x3bc;

// The frame rate patches call a code cave in place of the player's frame step: it adds 30 frames
// a second of the frame time to a remainder, once per game frame, and returns the whole frames.
constexpr VAddr StepCall = 0x10f741f;
constexpr std::array<u8, 5> StepCallBytes{0xe8, 0xfc, 0x6d, 0x41, 0xff};
constexpr VAddr StepCave = 0x50e220;
constexpr std::array<u8, 16> StepCaveBytes{0x8b, 0x05, 0x66, 0xce, 0x9e, 0x01, 0x3b, 0x05,
                                           0x84, 0xe4, 0xa6, 0x01, 0x0f, 0x84, 0x42, 0x00};
// vmovss [0x1f7c638], xmm0: the cave's store of the part of a frame left over.
constexpr VAddr RemainderStore = 0x50e265;
constexpr std::array<u8, 8> RemainderStoreBytes{0xc5, 0xfa, 0x11, 0x05, 0xcb, 0xe3, 0xa6, 0x01};
constexpr VAddr Remainder = 0x1f7c638;

// After the cave's call, the patch compares the playing flag: cmp byte [r12 + 0x3bc], 0; nop.
// Only a node that steps this game frame gets there.
constexpr Hooks::Entry Stepped{
    0x10f7424, 10, {0x41, 0x80, 0xbc, 0x24, 0xbc, 0x03, 0x00, 0x00, 0x00, 0x90}};
constexpr VAddr SteppedNext = 0x10f742e;

// A track samples when its node stepped or its clip changed: setne bl; or bl, r10b; jmp.
constexpr Hooks::Entry Gate{0x10f77a7, 8, {0x0f, 0x95, 0xc3, 0x44, 0x08, 0xd3, 0xeb, 0x02}};
constexpr VAddr GateNext = 0x10f77b1;

// An actor's and a camera's clip time in 1/3000 s: (clip offset + frame in clip) * 100.
constexpr Hooks::Entry ActorClip{
    0x10f7ac2, 10, {0x41, 0x03, 0x81, 0xb0, 0x00, 0x00, 0x00, 0x6b, 0xc0, 0x64}};
constexpr VAddr ActorClipNext = 0x10f7acc;
constexpr Hooks::Entry CameraClip{0x10f8590, 5, {0x03, 0x07, 0x6b, 0xc0, 0x64}};
constexpr VAddr CameraClipNext = 0x10f8595;
constexpr u32 TicksPerFrame = 100;

// The curve sampler FUN_014eff80(frame, object, name, curves), which only the player calls.
constexpr Hooks::Entry Curves{0x14eff80, 6, Hooks::FramePrologue};

// A node's tracks: pointers from +0x388 to +0x390. A track's frame in its clip is at +0xb0.
constexpr u64 NodeTracks = 0x388;
constexpr u64 NodeTracksEnd = 0x390;
constexpr u64 TrackFrame = 0xb0;
constexpr u64 MaxTracks = 512;

// A demo actor's own update, FUN_010f18e0, places the actor from its track's clips at the track's
// frame: the root clip (add eax, [rbx + 0xb0]; imul eax, eax, 100), the property clip
// (imul eax, [rax + 0xb0], 100) and the spin (imul edx, [rcx + 0xb0], 100).
constexpr Hooks::Entry ActorRoot{
    0x10f19e1, 9, {0x03, 0x83, 0xb0, 0x00, 0x00, 0x00, 0x6b, 0xc0, 0x64}};
constexpr VAddr ActorRootNext = 0x10f19ea;
constexpr Hooks::Entry ActorProperties{0x10f255f, 7, {0x6b, 0x80, 0xb0, 0x00, 0x00, 0x00, 0x64}};
constexpr VAddr ActorPropertiesNext = 0x10f2566;
constexpr Hooks::Entry ActorSpin{0x10f2601, 7, {0x6b, 0x91, 0xb0, 0x00, 0x00, 0x00, 0x64}};
constexpr VAddr ActorSpinNext = 0x10f2608;

// The game frame the step cave stamps its remainder with.
constexpr VAddr GameFrame = 0x1efb08c;

// void FUN_009cfa80(node, data): when node is the active camera node, copies its camera data into
// the camera switch's state and, outside a switch's blend, into the camera parameter block.
constexpr Hooks::Entry Apply{0x9cfa80, 5, {0x55, 0x48, 0x89, 0xe5, 0x53}};
constexpr VAddr ActiveNode = 0x1ec3b20;
// A timeline node's camera data, which FUN_010f8a50 applies. Eye at +0x10 and target at +0x20 of
// the 0xd0 bytes; their vector loads need 32-byte alignment.
constexpr u64 NodeCamera = 0x3c0;
constexpr size_t CameraBytes = 0xd0;
constexpr size_t Eye = 0x10;
constexpr size_t Target = 0x20;
// The camera a node holds before its first sample: eye (0, 0, 10) looking at the origin.
constexpr std::array<float, 3> UnsetEye{0.0f, 0.0f, 10.0f};
constexpr std::array<float, 3> UnsetTarget{0.0f, 0.0f, 0.0f};

using CurvesFunction = void PS4_SYSV_ABI (*)(float frame, u64 object, u32 name, u64 curves);
using ApplyFunction = void PS4_SYSV_ABI (*)(u64 node, u64 data);

VAddr g_base = 0;
CurvesFunction g_curves = nullptr;
ApplyFunction g_apply = nullptr;
std::mutex g_camera_mutex;
alignas(32) std::array<u8, CameraBytes> g_last_camera{};
bool g_has_last_camera = false;

// The node the player runs on this thread, and whether it stepped this game frame.
thread_local u64 t_node = 0;
thread_local bool t_stepped = false;

// Each track's node and the game frame the player last ran it, and each node's last step, for
// the demo actors, which update apart from the player.
struct TrackNode {
    u64 node;
    u32 seen;
};
std::mutex g_tracks_mutex;
std::unordered_map<u64, TrackNode> g_tracks;
std::unordered_map<u64, u32> g_steps;

template <typename T>
T Read(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

u8* Code(VAddr address) {
    return reinterpret_cast<u8*>(g_base + address - GoldHenBase);
}

template <size_t N>
bool Holds(VAddr address, const std::array<u8, N>& bytes) {
    return std::memcmp(Code(address), bytes.data(), N) == 0;
}

// Whether node plays within its range.
bool Plays(u64 node) {
    const s32 count = Read<s32>(node + NodeCount);
    return Read<u8>(node + NodePlaying) != 0 && Read<s32>(node + NodeStart) <= count &&
           count < Read<s32>(node + NodeEnd);
}

// Whether the node on this thread stepped this game frame and plays within its range.
bool Playing() {
    return t_stepped && Plays(t_node);
}

// The part of a frame since the node's count last moved.
float Fraction() {
    const float remainder = Read<float>(g_base + Remainder - GoldHenBase);
    return remainder > 0.0f ? std::min(remainder, 0.999f) : 0.0f;
}

u32 CurrentFrame() {
    return Read<u32>(g_base + GameFrame - GoldHenBase);
}

void PS4_SYSV_ABI OnPlayer(u32, const Hooks::Registers* registers, u64) {
    const u64 node = registers->rdi;
    t_node = node;
    t_stepped = false;
    const u64 begin = Read<u64>(node + NodeTracks);
    const u64 end = Read<u64>(node + NodeTracksEnd);
    if (begin == 0 || end < begin || end - begin > MaxTracks * sizeof(u64)) {
        return;
    }
    const u32 frame = CurrentFrame();
    std::scoped_lock lock{g_tracks_mutex};
    // Tracks and nodes the player no longer runs are dropped now and then.
    if (g_tracks.size() > 4 * MaxTracks) {
        std::erase_if(g_tracks,
                      [frame](const auto& entry) { return frame - entry.second.seen > 2; });
        std::erase_if(g_steps, [frame](const auto& entry) { return frame - entry.second > 2; });
    }
    for (u64 at = begin; at < end; at += sizeof(u64)) {
        const u64 track = Read<u64>(at);
        if (track != 0) {
            g_tracks[track] = {node, frame};
        }
    }
}

void PS4_SYSV_ABI OnStepped(u64 node) {
    t_stepped = node == t_node;
    std::scoped_lock lock{g_tracks_mutex};
    g_steps[node] = CurrentFrame();
}

// The ticks to add to a demo actor's clip times: its track's node played and stepped within the
// last game frame. The actor's update may run before or after the player in a game frame.
u32 PS4_SYSV_ABI ActorTicks(u64 track) {
    const u32 frame = CurrentFrame();
    u64 node = 0;
    {
        std::scoped_lock lock{g_tracks_mutex};
        const auto entry = g_tracks.find(track);
        if (entry == g_tracks.end() || frame - entry->second.seen > 1) {
            return 0;
        }
        node = entry->second.node;
        const auto step = g_steps.find(node);
        if (step == g_steps.end() || frame - step->second > 1) {
            return 0;
        }
    }
    return Plays(node) ? static_cast<u32>(Fraction() * TicksPerFrame) : 0;
}

u32 PS4_SYSV_ABI OpenGate() {
    return Playing() ? 1 : 0;
}

u32 PS4_SYSV_ABI ClipTicks() {
    return Playing() ? static_cast<u32>(Fraction() * TicksPerFrame) : 0;
}

void PS4_SYSV_ABI OnCurves(float frame, u64 object, u32 name, u64 curves) {
    if (Playing() && frame == static_cast<float>(Read<s32>(t_node + NodeCount))) {
        frame = std::min(frame + Fraction(), static_cast<float>(Read<s32>(t_node + NodeEnd)));
    }
    g_curves(frame, object, name, curves);
}

bool Unset(const u8* camera) {
    std::array<float, 3> eye;
    std::array<float, 3> target;
    std::memcpy(eye.data(), camera + Eye, sizeof(eye));
    std::memcpy(target.data(), camera + Target, sizeof(target));
    return eye == UnsetEye && target == UnsetTarget;
}

void PS4_SYSV_ABI OnApply(u64 node, u64 data) {
    if (Read<u64>(g_base + ActiveNode - GoldHenBase) != node || data == 0) {
        g_apply(node, data);
        return;
    }
    std::scoped_lock lock{g_camera_mutex};
    const auto* camera = reinterpret_cast<const u8*>(data);
    if (data == node + NodeCamera && Unset(camera) && g_has_last_camera) {
        g_apply(node, reinterpret_cast<u64>(g_last_camera.data()));
        return;
    }
    std::memcpy(g_last_camera.data(), camera, CameraBytes);
    g_has_last_camera = true;
    g_apply(node, data);
}

enum class Use { None, AddToEax, AddToEdx, OrIntoBl };

// The stack slot a stub pushed before EmitCall, which can hold the argument.
const Xbyak::Reg64* const PushedArgument = &Xbyak::util::rsp;

// Calls function between the game's instructions: saves the caller-saved general registers and
// every vector register, aligns the stack, passes argument (or the pushed slot) in rdi, and adds or
// ors the result into the game's eax, edx or bl.
void EmitCall(Xbyak::CodeGenerator& c, const void* function, const Xbyak::Reg64* argument,
              Use use) {
    using namespace Xbyak::util;
    const std::array<Xbyak::Reg64, 9> saved{rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11};
    constexpr s32 RaxSlot = 8 * 9;
    constexpr s32 RdxSlot = 8 * 7;
    constexpr s32 PushedSlot = 8 * 10;
    constexpr u32 VectorRegisters = 16;
    for (const auto& reg : saved) {
        c.push(reg);
    }
    c.push(rbp);
    c.mov(rbp, rsp);
    c.sub(rsp, VectorRegisters * 16);
    c.and_(rsp, ~u32{15});
    for (u32 i = 0; i < VectorRegisters; i++) {
        c.movdqu(ptr[rsp + i * 16], Xbyak::Xmm(static_cast<int>(i)));
    }
    if (argument == PushedArgument) {
        c.mov(rdi, qword[rbp + PushedSlot]);
    } else if (argument != nullptr) {
        c.mov(rdi, *argument);
    }
    c.mov(rax, reinterpret_cast<u64>(function));
    c.call(rax);
    if (use == Use::AddToEax) {
        c.add(dword[rbp + RaxSlot], eax);
    } else if (use == Use::AddToEdx) {
        c.add(dword[rbp + RdxSlot], eax);
    } else if (use == Use::OrIntoBl) {
        c.or_(bl, al);
    }
    for (u32 i = 0; i < VectorRegisters; i++) {
        c.movdqu(Xbyak::Xmm(static_cast<int>(i)), ptr[rsp + i * 16]);
    }
    c.mov(rsp, rbp);
    c.pop(rbp);
    for (auto reg = saved.rbegin(); reg != saved.rend(); ++reg) {
        c.pop(*reg);
    }
}

// Writes a stub that adds the clip ticks after the player's clip time and jumps to next.
const void* ClipStub(Xbyak::CodeGenerator& c, const Xbyak::Address& frame_in_clip, VAddr next) {
    using namespace Xbyak::util;
    const void* stub = c.getCurr();
    c.add(eax, frame_in_clip);
    c.imul(eax, eax, TicksPerFrame);
    EmitCall(c, reinterpret_cast<const void*>(&ClipTicks), nullptr, Use::AddToEax);
    c.jmp(Code(next));
    return stub;
}

bool Hooked(Hooks::Result result) {
    return result == Hooks::Result::Hooked || result == Hooks::Result::Chained;
}

bool InstallSampling(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (!Holds(StepCall, StepCallBytes) || !Holds(StepCave, StepCaveBytes) ||
        !Holds(RemainderStore, RemainderStoreBytes)) {
        LOG_INFO(Render_Vulkan, "GR2port: event timelines step in whole frames without a frame "
                                "rate patch");
        return true;
    }
    for (const Hooks::Entry& entry :
         {Stepped, Gate, ActorClip, CameraClip, ActorRoot, ActorProperties, ActorSpin}) {
        if (std::memcmp(Code(entry.address), entry.bytes.data(), entry.size) != 0) {
            LOG_ERROR(Render_Vulkan, "GR2port: the timeline player is not the expected code");
            return false;
        }
    }
    // The seven stubs below take about 2.2 KB.
    if (capacity - c.getSize() < 3_KB) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the timeline player's hooks");
        return false;
    }
    const void* original = nullptr;
    if (!Hooked(Hooks::ObserveEntry(module_base, Player, c, capacity, &OnPlayer, 0)) ||
        Hooks::Replace(module_base, Curves, c, capacity, reinterpret_cast<const void*>(&OnCurves),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the timeline player is not the expected code");
        return false;
    }
    g_curves = reinterpret_cast<CurvesFunction>(const_cast<void*>(original));

    const void* gate = c.getCurr();
    Xbyak::Label open;
    c.setne(bl);
    c.or_(bl, r10b);
    c.test(bl, bl);
    c.jnz(open, Xbyak::CodeGenerator::T_NEAR);
    EmitCall(c, reinterpret_cast<const void*>(&OpenGate), nullptr, Use::OrIntoBl);
    c.L(open);
    c.jmp(Code(GateNext));
    const void* actor_clip = ClipStub(c, dword[r9 + 0xb0], ActorClipNext);
    const void* camera_clip = ClipStub(c, dword[rdi], CameraClipNext);
    // The demo actor's stubs: the track is in rbx, rax (pushed, as the product replaces it) and
    // rcx.
    const void* actor_root = c.getCurr();
    c.add(eax, dword[rbx + TrackFrame]);
    c.imul(eax, eax, TicksPerFrame);
    EmitCall(c, reinterpret_cast<const void*>(&ActorTicks), &rbx, Use::AddToEax);
    c.jmp(Code(ActorRootNext));
    const void* actor_properties = c.getCurr();
    c.push(rax);
    c.imul(eax, dword[rax + TrackFrame], TicksPerFrame);
    EmitCall(c, reinterpret_cast<const void*>(&ActorTicks), PushedArgument, Use::AddToEax);
    c.lea(rsp, ptr[rsp + 8]);
    c.jmp(Code(ActorPropertiesNext));
    const void* actor_spin = c.getCurr();
    c.imul(edx, dword[rcx + TrackFrame], TicksPerFrame);
    EmitCall(c, reinterpret_cast<const void*>(&ActorTicks), &rcx, Use::AddToEdx);
    c.jmp(Code(ActorSpinNext));
    const void* stepped = c.getCurr();
    EmitCall(c, reinterpret_cast<const void*>(&OnStepped), &r12, Use::None);
    c.cmp(byte[r12 + NodePlaying], 0);
    c.jmp(Code(SteppedNext));

    // The step's mark goes in last: without it, no node samples between frames.
    if (Hooks::Replace(module_base, Gate, c, capacity, gate) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, ActorClip, c, capacity, actor_clip) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, CameraClip, c, capacity, camera_clip) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, ActorRoot, c, capacity, actor_root) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, ActorProperties, c, capacity, actor_properties) !=
            Hooks::Result::Hooked ||
        Hooks::Replace(module_base, ActorSpin, c, capacity, actor_spin) != Hooks::Result::Hooked ||
        Hooks::Replace(module_base, Stepped, c, capacity, stepped) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the timeline player's hooks");
        return false;
    }
    LOG_INFO(Render_Vulkan, "GR2port: event timelines sample between whole frames");
    return true;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    const void* original = nullptr;
    if (Hooks::Replace(module_base, Apply, c, capacity, reinterpret_cast<const void*>(&OnApply),
                       &original) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the camera node's application is not the expected code");
        return false;
    }
    g_apply = reinterpret_cast<ApplyFunction>(const_cast<void*>(original));
    return InstallSampling(module_base, c, capacity);
}

} // namespace Gr2Port::Timeline
