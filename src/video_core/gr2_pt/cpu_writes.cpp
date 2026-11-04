// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

#include "video_core/gr2_pt/cpu_writes.h"
#include "video_core/gr2_pt/gr2_pt.h"

namespace Gr2Pt::CpuWrites {

namespace {

// One bit per block, indexed by the block number modulo the bit count: blocks that share a bit
// report each other's writes, which only costs the renderer a lookup.
constexpr u64 WatchBits = u64{1} << 20;

std::array<std::atomic<u64>, WatchBits / 64> g_watched{};
std::mutex g_mutex;
std::vector<Range> g_writes;
std::atomic<bool> g_pending{false};

bool Watched(u64 block) {
    const u64 bit = block % WatchBits;
    return ((g_watched[bit / 64].load(std::memory_order_acquire) >> (bit % 64)) & 1) != 0;
}

} // namespace

void Watch(VAddr address, u64 size) {
    const u64 first = address >> BlockBits;
    const u64 last = (address + size - 1) >> BlockBits;
    for (u64 block = first; block <= last && block - first < WatchBits; ++block) {
        const u64 bit = block % WatchBits;
        g_watched[bit / 64].fetch_or(u64{1} << (bit % 64), std::memory_order_seq_cst);
    }
}

void Take(std::vector<Range>& writes) {
    writes.clear();
    if (!g_pending.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    std::swap(writes, g_writes);
    g_pending.store(false, std::memory_order_relaxed);
}

} // namespace Gr2Pt::CpuWrites

namespace Gr2Pt {

void OnCpuWrite(VAddr address, u64 size) {
    using namespace CpuWrites;
    if (size == 0) {
        return;
    }
    const u64 first = address >> BlockBits;
    const u64 last = (address + size - 1) >> BlockBits;
    bool watched = last - first >= WatchBits;
    for (u64 block = first; block <= last && !watched; ++block) {
        watched = Watched(block);
    }
    if (!watched) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    g_writes.push_back({address, address + size});
    g_pending.store(true, std::memory_order_release);
}

} // namespace Gr2Pt
