// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>

#include "video_core/gr2port/scene_ring.h"

namespace Gr2Port::SceneRing {

namespace {

enum State : u32 {
    Free,
    Writing,
    Ready,
    InUse,
};

struct Slot {
    std::atomic<u32> state{Free};
    std::atomic<u64> frame{0};
    std::atomic<bool> keep{false};
};

std::array<Slot, Slots> g_slots;
std::atomic<u8*> g_cpu{nullptr};
u64 g_address = 0;
std::atomic<u64> g_published{0};
std::atomic<u64> g_skipped{0};
// The slot the draw thread is writing; only the draw thread touches it.
u32 g_writing = Slots;

} // namespace

void Attach(u8* cpu, u64 address) {
    g_address = address;
    g_cpu.store(cpu, std::memory_order_release);
}

u8* Begin(bool keep) {
    u8* const cpu = g_cpu.load(std::memory_order_acquire);
    if (cpu == nullptr) {
        return nullptr;
    }
    // Other frames leave the last free slot to kept ones.
    u32 free = 0;
    for (const Slot& slot : g_slots) {
        free += slot.state.load(std::memory_order_relaxed) == Free ? 1 : 0;
    }
    if (free < (keep ? 1u : 2u)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    for (u32 i = 0; i < Slots; i++) {
        u32 expected = Free;
        if (g_slots[i].state.compare_exchange_strong(expected, Writing,
                                                     std::memory_order_acquire)) {
            g_writing = i;
            return cpu + i * SlotBytes;
        }
    }
    g_skipped.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

u64 Address(const u8* data) {
    return g_address + static_cast<u64>(data - g_cpu.load(std::memory_order_relaxed));
}

void Publish(u64 frame, bool keep) {
    // Empties the CPU's write-combining buffers: a plain store could pass the packet's last writes.
    std::atomic_thread_fence(std::memory_order_seq_cst);
    Slot& slot = g_slots[g_writing];
    slot.frame.store(frame, std::memory_order_relaxed);
    slot.keep.store(keep, std::memory_order_relaxed);
    slot.state.store(Ready, std::memory_order_release);
    g_published.store(frame, std::memory_order_release);
    g_published.notify_one();
}

bool Take(u64 seen, Packet& packet) {
    g_published.wait(seen, std::memory_order_acquire);
    u32 newest = Slots;
    u64 newest_frame = 0;
    u32 kept = Slots;
    u64 kept_frame = 0;
    for (u32 i = 0; i < Slots; i++) {
        if (g_slots[i].state.load(std::memory_order_acquire) != Ready) {
            continue;
        }
        const u64 frame = g_slots[i].frame.load(std::memory_order_relaxed);
        if (newest == Slots || frame > newest_frame) {
            newest = i;
            newest_frame = frame;
        }
        if (g_slots[i].keep.load(std::memory_order_relaxed) &&
            (kept == Slots || frame < kept_frame)) {
            kept = i;
            kept_frame = frame;
        }
    }
    if (newest == Slots) {
        return false;
    }
    if (kept != Slots) {
        newest = kept;
        newest_frame = kept_frame;
    }
    for (u32 i = 0; i < Slots; i++) {
        if (i != newest && g_slots[i].frame.load(std::memory_order_relaxed) > newest_frame) {
            continue;
        }
        u32 expected = Ready;
        g_slots[i].state.compare_exchange_strong(expected, i == newest ? InUse : Free,
                                                 std::memory_order_acq_rel);
    }
    packet = {
        .slot = newest,
        .frame = newest_frame,
        .data = g_cpu.load(std::memory_order_relaxed) + newest * SlotBytes,
        .address = g_address + newest * SlotBytes,
    };
    return true;
}

void Release(u32 slot) {
    g_slots[slot].state.store(Free, std::memory_order_release);
}

u64 Skipped() {
    return g_skipped.load(std::memory_order_relaxed);
}

} // namespace Gr2Port::SceneRing
