// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/capture_math.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/movies.h"

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
constexpr u32 OpDmaData = 0x50;
// FUN_01519d40(player) returns a texture object that keeps the player's frame: its memory object
// at +0x28 begins with the surface, which a DMA in the command buffer of the same kind at
// 0x1f58778 fills from the frame's surface.
constexpr Hooks::Entry Keep{0x1519d40, 6, Hooks::FramePrologue};
constexpr VAddr KeepCommands = 0x1f58778;
constexpr u64 KeptMemory = 0x28;
constexpr u32 ComputeUserData = 0x240;
constexpr u32 UserDataRegisters = 16;
// The job's constants.
constexpr u32 Width = 1;
constexpr u32 Height = 2;
constexpr u32 SourcePitch = 3;
constexpr u32 SourceHeight = 4;
constexpr u32 CropLeft = 5;
constexpr u32 CropTop = 6;

constexpr u32 KeptSurfaces = 8;
// The bytes of a surface's copy: AvPlayer's NV12 frame of up to 1920x1088, luma and chroma.
constexpr u64 FrameBytes = 1920 * 1088 * 3 / 2;
// A frame buffer goes back once its slot is unwritten this long, and is deleted GraceSeconds later.
constexpr u32 IdleSeconds = 30;
constexpr u32 GraceSeconds = 5;
constexpr u32 LoggedJobs = 4;
constexpr u32 LoggedKeeps = 8;

// What the game's GPU work last wrote into one surface, taken on the game's thread in the game's
// order: the job's constants and a copy of the frame in AvPlayer's layout. A kept surface also
// names the surface its DMA copied. Read from any thread: the sequence is odd while the words
// change, and a new frame goes into the buffer readers do not read. The generation counts the
// writes of every slot.
struct Slot {
    std::atomic<u32> sequence{0};
    std::atomic<u32> generation{0};
    std::atomic<VAddr> surface{0};
    std::atomic<VAddr> source{0};
    std::atomic<bool> copied{false};
    std::atomic<u32> published{0};
    std::array<std::atomic<u32>, 7> constants{};
    std::array<std::unique_ptr<u8[]>, 2> pixels;
    /// Seconds since the start at the last copy into the slot.
    std::atomic<u32> written{0};
};

// A frame buffer the collector took, deleted once no reader can still hold it.
struct Freed {
    std::unique_ptr<u8[]> buffer;
    u32 at;
};

VAddr g_base = 0;
// The copies, the publishes and the collector take this; readers use the published buffer only.
std::mutex g_buffers;
std::vector<Freed> g_freed;
const auto g_start = std::chrono::steady_clock::now();
std::array<Slot, Surfaces> g_slots;
std::array<Slot, KeptSurfaces> g_kept;
u32 g_next = 0;
u32 g_next_kept = 0;
u32 g_logged = 0;
u32 g_logged_keeps = 0;
u32 g_logged_copies = 0;
u32 g_generation = 0;

// The slot of surface, or the next one round the slots.
template <typename Slots>
Slot& SlotOf(Slots& slots, u32& next, VAddr surface) {
    const auto found = std::ranges::find_if(slots, [surface](const Slot& slot) {
        return slot.surface.load(std::memory_order_relaxed) == surface;
    });
    if (found != slots.end()) {
        return *found;
    }
    Slot& slot = slots[next];
    next = (next + 1) % slots.size();
    return slot;
}

u32 Seconds() {
    return static_cast<u32>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_start)
            .count());
}

// Under g_buffers: gives the collector the back buffer of a slot unwritten for IdleSeconds, and
// both buffers of a slot no surface owns.
template <typename Slots>
void CollectSlots(Slots& slots, u32 now) {
    for (Slot& slot : slots) {
        const bool owned = slot.surface.load(std::memory_order_relaxed) != 0;
        const u32 published = slot.published.load(std::memory_order_relaxed);
        if (owned && now - slot.written.load(std::memory_order_relaxed) < IdleSeconds) {
            continue;
        }
        for (u32 i = 0; i < slot.pixels.size(); i++) {
            if ((owned && i == published) || !slot.pixels[i]) {
                continue;
            }
            g_freed.push_back({std::move(slot.pixels[i]), now});
        }
    }
}

// Under g_buffers, held until the copy is published: copies bytes from data into the buffer of
// slot readers do not read; false when they do not fit.
bool CopyFrame(Slot& slot, const void* data, u64 bytes) {
    if (data == nullptr || bytes == 0 || bytes > FrameBytes) {
        return false;
    }
    slot.written.store(Seconds(), std::memory_order_relaxed);
    auto& buffer = slot.pixels[slot.published.load(std::memory_order_relaxed) ^ 1];
    if (!buffer) {
        buffer = std::make_unique<u8[]>(FrameBytes);
    }
    const auto start = std::chrono::steady_clock::now();
    std::memcpy(buffer.get(), data, bytes);
    if (g_logged_copies < LoggedKeeps) {
        g_logged_copies++;
        LOG_INFO(Render_Vulkan, "GR2port movies: frame copied, {} bytes in {} us", bytes,
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count());
    }
    return true;
}

// Under g_buffers.
void Publish(Slot& slot, VAddr surface, VAddr source, bool copied,
             const std::array<u32, 7>& values) {
    const u32 sequence = slot.sequence.load(std::memory_order_relaxed);
    slot.sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    slot.generation.store(++g_generation, std::memory_order_relaxed);
    slot.surface.store(surface, std::memory_order_relaxed);
    slot.source.store(source, std::memory_order_relaxed);
    slot.copied.store(copied, std::memory_order_relaxed);
    if (copied) {
        slot.published.store(slot.published.load(std::memory_order_relaxed) ^ 1,
                             std::memory_order_relaxed);
        for (u32 i = 0; i < values.size(); i++) {
            slot.constants[i].store(values[i], std::memory_order_relaxed);
        }
    }
    slot.sequence.store(sequence + 2, std::memory_order_release);
}

// The published frame of slot, read under its sequence.
Frame Read(const Slot& slot, VAddr surface, VAddr* source = nullptr) {
    Frame frame{};
    u32 before = 0;
    do {
        before = slot.sequence.load(std::memory_order_acquire);
        frame.found = slot.copied.load(std::memory_order_relaxed);
        frame.generation = slot.generation.load(std::memory_order_relaxed);
        frame.source = reinterpret_cast<VAddr>(
            slot.pixels[slot.published.load(std::memory_order_relaxed)].get());
        for (u32 i = 0; i < frame.constants.size(); i++) {
            frame.constants[i] = slot.constants[i].load(std::memory_order_relaxed);
        }
        if (source != nullptr) {
            *source = slot.source.load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
    } while ((before & 1) != 0 || slot.sequence.load(std::memory_order_relaxed) != before);
    return slot.surface.load(std::memory_order_relaxed) == surface ? frame : Frame{};
}

// A surface is a movie's or a kept frame's, whichever the game wrote last: AvPlayer and the keeps
// share their memory, so a new owner of an address drops the slot of the old one.
template <typename Slots>
void Drop(Slots& slots, VAddr surface) {
    for (auto& slot : slots) {
        if (slot.surface.load(std::memory_order_relaxed) != surface) {
            continue;
        }
        const u32 sequence = slot.sequence.load(std::memory_order_relaxed);
        slot.sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.surface.store(0, std::memory_order_relaxed);
        slot.sequence.store(sequence + 2, std::memory_order_release);
    }
}

using UserData = std::array<u32, UserDataRegisters>;

// Calls packet(opcode, count, body) for each type 3 packet a command buffer holds.
template <typename Packet>
void Walk(VAddr commands, Packet&& packet) {
    const VAddr start = Load<VAddr>(commands + CommandsStart);
    const VAddr next = Load<VAddr>(commands + CommandsNext);
    if (start == 0 || next <= start || next - start > MaxCommandBytes) {
        return;
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
        if (type == 3 && at + count + 2 <= stop) {
            packet(header >> 8 & 0xff, count, at + 1);
        }
        at += count + 2;
    }
}

// The compute user data registers the job's commands set.
UserData ReadUserData() {
    UserData user{};
    Walk(g_base + Commands - GoldHenBase, [&user](u32 opcode, u32 count, const u32* body) {
        if (opcode != OpSetShReg) {
            return;
        }
        const u32 first = body[0] & 0xffff;
        for (u32 i = 0; i < count; i++) {
            const u32 index = first + i - ComputeUserData;
            if (first + i >= ComputeUserData && index < UserDataRegisters) {
                user[index] = body[1 + i];
            }
        }
    });
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
    // AvPlayer reuses its frame two decodes later, so the frame is copied now, as the job reads it.
    Drop(g_kept, surface);
    Slot& slot = SlotOf(g_slots, g_next, surface);
    const auto values = Load<std::array<u32, 7>>(constants);
    // The collector takes the buffers of a slot no surface owns, so it waits for the publish.
    std::scoped_lock lock{g_buffers};
    const bool copied = CopyFrame(slot, reinterpret_cast<const void*>(source),
                                  u64{values[SourcePitch]} * values[SourceHeight] * 3 / 2);
    Publish(slot, surface, 0, copied, values);
}

// DMA_DATA holds the source's address in words 1 and 2 of its body, the destination's in 3 and 4.
void PS4_SYSV_ABI OnKeep(u32, const Hooks::Arguments*, u64 result) {
    const VAddr memory = result != 0 ? Load<VAddr>(result + KeptMemory) : 0;
    const VAddr surface = memory != 0 ? Load<VAddr>(memory) : 0;
    if (surface == 0) {
        return;
    }
    VAddr source = 0;
    Walk(g_base + KeepCommands - GoldHenBase,
         [surface, &source](u32 opcode, u32 count, const u32* body) {
             if (opcode != OpDmaData || count < 5 || source != 0) {
                 return;
             }
             if ((u64{body[3]} | u64{body[4]} << 32) == surface) {
                 source = u64{body[1]} | u64{body[2]} << 32;
             }
         });
    if (g_logged_keeps < LoggedKeeps) {
        g_logged_keeps++;
        LOG_INFO(Render_Vulkan, "GR2port movies: frame of {:#x} kept in {:#x}", source, surface);
    }
    if (source == 0) {
        return;
    }
    // The DMA copies what the source holds now: the copy of the source's last job.
    Drop(g_slots, surface);
    Slot& slot = SlotOf(g_kept, g_next_kept, surface);
    std::scoped_lock lock{g_buffers};
    const auto job = std::ranges::find_if(g_slots, [source](const Slot& job) {
        return job.surface.load(std::memory_order_relaxed) == source &&
               job.copied.load(std::memory_order_relaxed);
    });
    std::array<u32, 7> values{};
    bool copied = false;
    if (job != g_slots.end()) {
        for (u32 i = 0; i < values.size(); i++) {
            values[i] = job->constants[i].load(std::memory_order_relaxed);
        }
        copied = CopyFrame(slot, job->pixels[job->published.load(std::memory_order_relaxed)].get(),
                           u64{values[SourcePitch]} * values[SourceHeight] * 3 / 2);
    }
    Publish(slot, surface, source, copied, values);
}

bool Hooked(Hooks::Result result) {
    return result == Hooks::Result::Hooked || result == Hooks::Result::Chained;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    return Hooked(Hooks::ObserveReturn(module_base, Update, c, capacity, &OnUpdate, 0)) &&
           Hooked(Hooks::ObserveReturn(module_base, Keep, c, capacity, &OnKeep, 0));
}

Job Peek(u32 slot) {
    const Slot& s = g_slots[slot];
    Job job{};
    u32 before = 0;
    do {
        before = s.sequence.load(std::memory_order_acquire);
        job.surface = s.surface.load(std::memory_order_relaxed);
        job.generation = s.generation.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
    } while ((before & 1) != 0 || s.sequence.load(std::memory_order_relaxed) != before);
    return job;
}

Kept FindKept(VAddr surface) {
    for (const Slot& slot : g_kept) {
        if (slot.surface.load(std::memory_order_relaxed) != surface) {
            continue;
        }
        Kept kept{};
        kept.frame = Read(slot, surface, &kept.source);
        kept.generation = kept.frame.generation;
        return kept.source != 0 ? kept : Kept{};
    }
    return {};
}

void Collect() {
    std::scoped_lock lock{g_buffers};
    // Read under the lock, so no copy is later than it.
    const u32 now = Seconds();
    CollectSlots(g_slots, now);
    CollectSlots(g_kept, now);
    std::erase_if(g_freed, [now](const Freed& f) { return now - f.at >= GraceSeconds; });
}

Frame Find(VAddr surface) {
    for (u32 i = 0; i < Surfaces; i++) {
        const Slot& slot = g_slots[i];
        if (slot.surface.load(std::memory_order_relaxed) != surface) {
            continue;
        }
        Frame frame = Read(slot, surface);
        frame.slot = i;
        return frame;
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
