// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "core/tls.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/gr2_pt/gr2_pt.h"
#include "video_core/gr2_pt/marker.h"

namespace Gr2Pt::Marker {

namespace {

// Game code reads the thread's graphics context below its thread control block.
constexpr u64 TcbContext = 0x38;

// The context's command buffer, which the game's packet writers fill and grow.
constexpr u64 BufferEnd = 0x8;
constexpr u64 BufferCursor = 0x10;
constexpr u64 BufferReserve = 0x18;
constexpr u64 BufferReserveData = 0x20;

using ReserveFn = u8 PS4_SYSV_ABI (*)(VAddr buffer, u32 dwords, VAddr data);

constexpr u32 PacketDwords = 2 + WordCount;

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

} // namespace

VAddr Context() {
    return Load<VAddr>(reinterpret_cast<VAddr>(Core::GetTcbBase()) - TcbContext);
}

void Write(const Words& words) {
    const VAddr buffer = Context();
    auto* cursor = Load<u32*>(buffer + BufferCursor);
    if (Load<u32*>(buffer + BufferEnd) - cursor < PacketDwords) {
        const auto reserve = Load<ReserveFn>(buffer + BufferReserve);
        if (reserve(buffer, PacketDwords, Load<VAddr>(buffer + BufferReserveData)) == 0) {
            return;
        }
        cursor = Load<u32*>(buffer + BufferCursor);
    }
    cursor[0] = AmdGpu::PM4Type3Header{AmdGpu::PM4ItOpcode::Nop, PacketDwords - 2}.raw;
    cursor[1] = MarkerPayload;
    std::memcpy(cursor + 2, &words, sizeof(words));
    u32* const next = cursor + PacketDwords;
    std::memcpy(reinterpret_cast<void*>(buffer + BufferCursor), &next, sizeof(next));
}

} // namespace Gr2Pt::Marker
