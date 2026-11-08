// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>

#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "video_core/gr2port/capture.h"
#include "video_core/gr2port/capture_math.h"
#include "video_core/gr2port/hooks.h"
#include "video_core/gr2port/movies.h"

namespace Gr2Port::Movies {

namespace {

using Capture::Load;

constexpr VAddr GoldHenBase = 0x400000;
// FUN_01519400(player): returns 1 when it copied a new frame. The player holds the frame's record
// at +0x90 when done; a record holds its texture at +0x28, whose first word is the surface.
constexpr Hooks::Entry Update{0x1519400, 6, Hooks::FramePrologue};
constexpr u64 PlayerRecord = 0x90;
constexpr u64 RecordTexture = 0x28;
// The constant update engine of the movie job: the shader's table of where each resource goes at
// +0x18a8, and the user data it copies into the job at +0x98. The table's entries for the first
// read-only buffer, read-write buffer and constant buffer: an offset in dwords, with bit 15 set
// when the resource goes into a user data register instead, which the job's commands set.
constexpr VAddr Engine = 0x1f5a3f8;
constexpr u64 EngineTable = 0x18a8;
constexpr u64 EngineUserData = 0x98;
constexpr u64 TableSource = 0x11;
constexpr u64 TableDestination = 0x21;
constexpr u64 TableConstants = 0x41;
constexpr u16 Unused = 0xffff;
constexpr u16 InRegister = 0x8000;
constexpr u16 OffsetMask = 0x3fff;
// The movie job's command buffer: where its commands start at +0x1a38, and where the next goes
// at +0x20.
constexpr VAddr Commands = 0x1f5a3a8;
constexpr u64 CommandsStart = 0x1a38;
constexpr u64 CommandsNext = 0x20;
constexpr u64 MaxCommandBytes = 64 * 1024;
constexpr u32 OpSetShReg = 0x76;
constexpr u32 ComputeUserData = 0x240;
constexpr u32 UserDataRegisters = 16;
// The job's constants.
constexpr u32 Width = 1;
constexpr u32 Height = 2;
constexpr u32 SourcePitch = 3;
constexpr u32 SourceHeight = 4;
constexpr u32 CropLeft = 5;
constexpr u32 CropTop = 6;

constexpr u32 Surfaces = 8;
constexpr u32 LoggedJobs = 4;

// One surface's last job, written by the game's thread and read by any: the sequence is odd while
// the words change, and counts the jobs.
struct Slot {
    std::atomic<u32> sequence{0};
    std::atomic<VAddr> surface{0};
    std::atomic<VAddr> source{0};
    std::atomic<u64> frame{0};
    std::array<std::atomic<u32>, 7> constants{};
};

VAddr g_base = 0;
std::array<Slot, Surfaces> g_slots;
u32 g_next = 0;
u32 g_logged = 0;

using UserData = std::array<u32, UserDataRegisters>;

// The compute user data registers the job's commands set.
UserData ReadUserData() {
    UserData user{};
    const VAddr commands = g_base + Commands - GoldHenBase;
    const VAddr start = Load<VAddr>(commands + CommandsStart);
    const VAddr next = Load<VAddr>(commands + CommandsNext);
    if (start == 0 || next <= start || next - start > MaxCommandBytes) {
        return user;
    }
    const u32* at = reinterpret_cast<const u32*>(start);
    const u32* const stop = reinterpret_cast<const u32*>(next);
    while (at < stop) {
        const u32 header = *at;
        const u32 type = header >> 30;
        if (type == 2) {
            at++;
            continue;
        }
        const u32 count = header >> 16 & 0x3fff;
        if (type == 3 && (header >> 8 & 0xff) == OpSetShReg && at + count + 2 <= stop) {
            const u32 first = at[1] & 0xffff;
            for (u32 i = 0; i < count; i++) {
                const u32 index = first + i - ComputeUserData;
                if (first + i >= ComputeUserData && index < UserDataRegisters) {
                    user[index] = at[2 + i];
                }
            }
        }
        at += count + 2;
    }
    return user;
}

// The base of a resource's V#, wherever the engine put it; 0 for none.
VAddr ResourceBase(VAddr engine, VAddr table, u64 index, const UserData& user) {
    const u16 entry = Load<u16>(table + index * 2);
    if (entry == Unused) {
        return 0;
    }
    const u32 offset = entry & OffsetMask;
    std::array<u32, 2> words{};
    if ((entry & InRegister) != 0) {
        if (offset + 2 > UserDataRegisters) {
            return 0;
        }
        words = {user[offset], user[offset + 1]};
    } else {
        words = Load<std::array<u32, 2>>(engine + EngineUserData + u64{offset} * 4);
    }
    return u64{words[0]} | u64{words[1] & 0xfff} << 32;
}

void PS4_SYSV_ABI OnUpdate(u32, const Hooks::Arguments* arguments, u64 result) {
    if ((result & 0xff) == 0) {
        return;
    }
    const VAddr record = Load<VAddr>(arguments->rdi + PlayerRecord);
    const VAddr texture = record != 0 ? Load<VAddr>(record + RecordTexture) : 0;
    const VAddr surface = texture != 0 ? Load<VAddr>(texture) : 0;
    const VAddr engine = g_base + Engine - GoldHenBase;
    const VAddr table = Load<VAddr>(engine + EngineTable);
    const UserData user = ReadUserData();
    const VAddr source = ResourceBase(engine, table, TableSource, user);
    const VAddr destination = ResourceBase(engine, table, TableDestination, user);
    const VAddr constants = ResourceBase(engine, table, TableConstants, user);
    if (g_logged < LoggedJobs) {
        g_logged++;
        const auto values =
            constants != 0 ? Load<std::array<u32, 7>>(constants) : std::array<u32, 7>{};
        LOG_INFO(Render_Vulkan,
                 "GR2port movies: frame into {:#x} (job's destination {:#x}) from {:#x}, constants "
                 "{} {} {} {} {} {} {}; table entries {:#x} {:#x} {:#x}, user data {:x}",
                 surface, destination, source, values[0], values[1], values[2], values[3],
                 values[4], values[5], values[6], Load<u16>(table + TableSource * 2),
                 Load<u16>(table + TableDestination * 2), Load<u16>(table + TableConstants * 2),
                 fmt::join(user, " "));
    }
    if (surface == 0 || surface != destination || source == 0 || constants == 0) {
        return;
    }
    auto found = std::ranges::find_if(g_slots, [surface](const Slot& slot) {
        return slot.surface.load(std::memory_order_relaxed) == surface;
    });
    if (found == g_slots.end()) {
        found = g_slots.begin() + g_next;
        g_next = (g_next + 1) % Surfaces;
    }
    Slot& slot = *found;
    const u32 sequence = slot.sequence.load(std::memory_order_relaxed);
    slot.sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.surface.store(surface, std::memory_order_relaxed);
    slot.source.store(source, std::memory_order_relaxed);
    slot.frame.store(Capture::Frames(), std::memory_order_relaxed);
    const auto values = Load<std::array<u32, 7>>(constants);
    for (u32 i = 0; i < values.size(); i++) {
        slot.constants[i].store(values[i], std::memory_order_relaxed);
    }
    slot.sequence.store(sequence + 2, std::memory_order_release);
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    const Hooks::Result result =
        Hooks::ObserveReturn(module_base, Update, c, capacity, &OnUpdate, 0);
    return result == Hooks::Result::Hooked || result == Hooks::Result::Chained;
}

Frame Find(VAddr surface) {
    for (const Slot& slot : g_slots) {
        if (slot.surface.load(std::memory_order_relaxed) != surface) {
            continue;
        }
        Frame frame{.found = true};
        u32 before = 0;
        do {
            before = slot.sequence.load(std::memory_order_acquire);
            frame.source = slot.source.load(std::memory_order_relaxed);
            const u64 job_frame = slot.frame.load(std::memory_order_relaxed);
            frame.age = Capture::Frames() - std::min(job_frame, Capture::Frames());
            for (u32 i = 0; i < frame.constants.size(); i++) {
                frame.constants[i] = slot.constants[i].load(std::memory_order_relaxed);
            }
            std::atomic_thread_fence(std::memory_order_acquire);
        } while ((before & 1) != 0 || slot.sequence.load(std::memory_order_relaxed) != before);
        frame.generation = before;
        return frame.source != 0 && slot.surface.load(std::memory_order_relaxed) == surface
                   ? frame
                   : Frame{};
    }
    return {};
}

void Write(const Frame& frame, u8* out, u32 pitch, u32 rows) {
    const auto& c = frame.constants;
    const u32 width = std::min(c[Width], pitch);
    const u32 height = std::min(c[Height], rows);
    const u32 chroma_rows = std::min(c[Height] / 2, rows - height);
    const u8* const luma = reinterpret_cast<const u8*>(frame.source);
    const u8* const chroma = luma + u64{c[SourcePitch]} * c[SourceHeight];
    for (u32 y = 0; y < height; y++) {
        std::memcpy(out + u64{y} * pitch, luma + u64{y + c[CropTop]} * c[SourcePitch] + c[CropLeft],
                    width);
    }
    for (u32 y = 0; y < chroma_rows; y++) {
        std::memcpy(out + u64{height + y} * pitch,
                    chroma + u64{y + c[CropTop] / 2} * c[SourcePitch] + c[CropLeft], width);
    }
}

} // namespace Gr2Port::Movies
