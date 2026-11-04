// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include "common/logging/log.h"
#include "core/tls.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/gr2_pt/caster_batch.h"
#include "video_core/gr2_pt/hooks.h"
#include "video_core/gr2_pt/marker.h"
#include "video_core/gr2_pt/shadow_pass.h"

namespace Gr2Pt::CasterBatch {

namespace {

// The opaque caster item callback FUN_0141c510(out, item, view), which SubmitCategory calls for
// each item of category 0. It returns out, whose first word it sets from this global.
constexpr Hooks::Entry CasterItem{0x141c510, 6, Hooks::FramePrologue};
constexpr u64 CallbackResult = 0x1f150a0;
constexpr u64 CodeBase = 0x400000;

// One frame in CompareInterval runs the game's draws of every caster as well, for the renderer's
// check against them.
constexpr u32 CompareInterval = 600;

// The 64-byte caster item.
constexpr u64 ItemFlags = 0x4;
constexpr u64 ItemCascades = 0x5;
constexpr u64 ItemBones = 0x6;
constexpr u64 ItemMesh = 0x8;
constexpr u64 ItemStreams = 0x10;
constexpr u64 ItemSkinStreams = 0x18;
constexpr u64 ItemIndices = 0x20;
constexpr u64 ItemPalette = 0x28;
constexpr u64 ItemWorld = 0x30;
constexpr u64 ItemSubMesh = 0x38;
constexpr u8 FlagCullBack = 0x8;
constexpr u8 FlagProjection1 = 0x10;
constexpr u8 FlagProjection2 = 0x20;
// The view's cascade and its view-projection tables, which hold one pointer per cascade.
constexpr u64 ViewCascade = 0x740;
constexpr u64 ViewProjection0 = 0x748;
constexpr u64 ViewProjection1 = 0x760;
constexpr u64 ViewProjection2 = 0x778;
// A mesh's index range, and whether a caster without a sub-mesh draws it. A sub-mesh holds its
// range at the same offsets.
constexpr u64 MeshDraws = 0x1e;
constexpr u64 MeshFirst = 0x20;
constexpr u64 MeshBase = 0x40;
constexpr u64 MeshCount = 0x44;
// A stream set: its stream count, each stream's vertex slot in 4-byte entries and its V#.
constexpr u64 StreamCount = 0x1c;
constexpr u64 StreamSlots = 0x37;
constexpr u64 StreamBuffers = 0x64;
constexpr u32 MaxStreams = 16;
// An index object points to its range {address, bytes} and holds its index size (0 is 16-bit).
constexpr u64 IndexRangeBytes = 0x8;
constexpr u64 IndexType = 0x18;
// The vertex slots the caster programs read.
constexpr u8 PositionSlot = 0;
constexpr u8 WeightSlot = 1;
constexpr u8 BoneSlot = 7;
constexpr u32 MaxBones = 256;
// V# component selects X, Y, Z and W.
constexpr u32 SelectX = 4;
constexpr u32 SelectY = 5;
constexpr u32 SelectZ = 6;
constexpr u32 SelectW = 7;
// The context's count of issued draws, which DrawIndexed FUN_016173f0 increments.
constexpr u64 ContextDraws = 0xcec8;

using ItemCallback = u32* PS4_SYSV_ABI (*)(u32* out, VAddr item, VAddr view);

// Written only by the game's draw thread.
struct DrawThread {
    ItemCallback original;
    const u32* result;
    const void* tcb;
    bool active;
    bool compare;
    // A caster the game's callback drew opened the pass: its draw flushed the pass state.
    bool started;
    u32 cascade;
    u32 frame;
    u32 first;
    u32 written;
    // The cascade's casters after the first that the game's callback drew.
    u32 fallbacks;
};
DrawThread g_draw;

std::array<Record, RingRecords> g_ring;
// The end of the records the command processor has drawn.
std::atomic<u32> g_released;
std::atomic<bool> g_enabled;

std::atomic<u32> g_batched;
std::atomic<u32> g_unsupported;
std::atomic<u32> g_ring_full;

enum class Outcome {
    Batched,
    // The callback would draw nothing.
    Empty,
    Unsupported,
};

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

bool FindStream(VAddr set, u8 slot, AmdGpu::Buffer& buffer) {
    if (set == 0) {
        return false;
    }
    const u32 count = std::min<u32>(Load<u16>(set + StreamCount), MaxStreams);
    for (u32 i = 0; i < count; i++) {
        if (Load<u8>(set + StreamSlots + i * 4) == slot) {
            std::memcpy(&buffer, reinterpret_cast<const void*>(set + StreamBuffers + i * 16),
                        sizeof(buffer));
            return true;
        }
    }
    return false;
}

// The callback binds the skin set's streams after the main set's, so the skin set wins a slot.
bool FindStream(VAddr set, VAddr skin_set, u8 slot, AmdGpu::Buffer& buffer) {
    return FindStream(skin_set, slot, buffer) || FindStream(set, slot, buffer);
}

bool IsPlain(const AmdGpu::Buffer& buffer, bool four) {
    return buffer.base_address != 0 && buffer.num_records != 0 && buffer.stride % 4 == 0 &&
           buffer.base_address % 4 == 0 && !buffer.swizzle_enable && !buffer.add_tid_enable &&
           buffer.dst_sel_x == SelectX && buffer.dst_sel_y == SelectY &&
           buffer.dst_sel_z == SelectZ && (!four || buffer.dst_sel_w == SelectW);
}

bool IsFormat(const AmdGpu::Buffer& buffer, AmdGpu::DataFormat data, AmdGpu::NumberFormat number) {
    return buffer.data_format == static_cast<u32>(data) &&
           buffer.num_format == static_cast<u32>(number);
}

Outcome Fill(Record& r, VAddr item, VAddr view, u32 cascade) {
    const VAddr mesh = Load<VAddr>(item + ItemMesh);
    const VAddr indices = Load<VAddr>(item + ItemIndices);
    const VAddr sub = Load<VAddr>(item + ItemSubMesh);
    if (mesh == 0) {
        return Outcome::Unsupported;
    }
    s32 first;
    s32 count;
    if (sub == 0) {
        if (Load<u16>(mesh + MeshDraws) == 0) {
            return Outcome::Empty;
        }
        first = Load<s32>(mesh + MeshBase) + Load<s32>(mesh + MeshFirst);
        count = Load<s32>(mesh + MeshCount);
    } else {
        first = Load<s32>(mesh + MeshFirst);
        count = Load<s32>(sub + MeshBase) - (Load<s32>(mesh + MeshBase) + first) +
                Load<s32>(sub + MeshCount) + Load<s32>(sub + MeshFirst);
    }
    if (count <= 0) {
        return Outcome::Empty;
    }
    const u8 flags = Load<u8>(item + ItemFlags);
    const u64 table = (flags & FlagProjection1)   ? ViewProjection1
                      : (flags & FlagProjection2) ? ViewProjection2
                                                  : ViewProjection0;
    r.view_projection = Load<VAddr>(view + table + u64{cascade} * 8);
    r.world = Load<VAddr>(item + ItemWorld);
    const VAddr range = indices != 0 ? Load<VAddr>(indices) : 0;
    if (first < 0 || r.view_projection == 0 || r.world == 0 || range == 0) {
        return Outcome::Unsupported;
    }
    r.indices = Load<VAddr>(range);
    r.index_bytes = Load<u32>(range + IndexRangeBytes);
    const bool index32 = Load<u32>(indices + IndexType) != 0;
    const u64 index_size = index32 ? 4 : 2;
    if (r.indices == 0 || r.indices % index_size != 0 ||
        (u64{static_cast<u32>(first)} + static_cast<u32>(count)) * index_size > r.index_bytes) {
        return Outcome::Unsupported;
    }
    r.first = static_cast<u32>(first);
    r.count = static_cast<u32>(count);
    r.flags = static_cast<u8>(((flags & FlagCullBack) ? CullBack : 0) | (index32 ? Index32 : 0));

    using AmdGpu::DataFormat;
    using AmdGpu::NumberFormat;
    r.bones = Load<u16>(item + ItemBones);
    const VAddr set = Load<VAddr>(item + ItemStreams);
    const VAddr skin_set = r.bones != 0 ? Load<VAddr>(item + ItemSkinStreams) : 0;
    if (!FindStream(set, skin_set, PositionSlot, r.positions) || !IsPlain(r.positions, false) ||
        r.positions.stride < 12 ||
        !(IsFormat(r.positions, DataFormat::Format32_32_32, NumberFormat::Float) ||
          IsFormat(r.positions, DataFormat::Format32_32_32_32, NumberFormat::Float))) {
        return Outcome::Unsupported;
    }
    if (r.bones == 0) {
        r.palette = 0;
        return Outcome::Batched;
    }
    r.palette = Load<VAddr>(item + ItemPalette);
    if (r.bones > MaxBones || r.palette == 0 || !FindStream(set, skin_set, WeightSlot, r.weights) ||
        !IsPlain(r.weights, true) ||
        !IsFormat(r.weights, DataFormat::Format16_16_16_16, NumberFormat::Float) ||
        !FindStream(set, skin_set, BoneSlot, r.bone_indices) || !IsPlain(r.bone_indices, true) ||
        !(IsFormat(r.bone_indices, DataFormat::Format16_16_16_16, NumberFormat::Sint) ||
          IsFormat(r.bone_indices, DataFormat::Format16_16_16_16, NumberFormat::Uint))) {
        return Outcome::Unsupported;
    }
    return Outcome::Batched;
}

Outcome Append(VAddr item, VAddr view, u32 cascade) {
    DrawThread& t = g_draw;
    if (t.written - g_released.load(std::memory_order_acquire) >= RingRecords) {
        g_ring_full.fetch_add(1, std::memory_order_relaxed);
        ++t.fallbacks;
        return Outcome::Unsupported;
    }
    const Outcome outcome = Fill(g_ring[t.written % RingRecords], item, view, cascade);
    if (outcome == Outcome::Batched) {
        ++t.written;
    } else if (outcome == Outcome::Unsupported) {
        g_unsupported.fetch_add(1, std::memory_order_relaxed);
        ++t.fallbacks;
    }
    return outcome;
}

u32* PS4_SYSV_ABI OnItem(u32* out, VAddr item, VAddr view) {
    DrawThread& t = g_draw;
    if (!t.active || Core::GetTcbBase() != t.tcb) {
        return t.original(out, item, view);
    }
    const u32 cascade = Load<u32>(view + ViewCascade);
    if (((Load<u8>(item + ItemCascades) >> (cascade & 31)) & 1) == 0) {
        return t.original(out, item, view);
    }
    if (!t.started) {
        const VAddr context = Marker::Context();
        const u32 draws = Load<u32>(context + ContextDraws);
        u32* const result = t.original(out, item, view);
        t.started = Load<u32>(context + ContextDraws) != draws;
        if (t.started && t.compare) {
            Marker::Write({Marker::Kind::CasterSnapshot, ShadowPass::FirstPass + t.cascade, t.frame,
                           0, 0, 0, 0, 0});
        }
        return result;
    }
    if (t.compare) {
        u32* const result = t.original(out, item, view);
        Append(item, view, cascade);
        return result;
    }
    if (Append(item, view, cascade) == Outcome::Unsupported) {
        return t.original(out, item, view);
    }
    *out = *t.result;
    return out;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_draw.result = reinterpret_cast<const u32*>(module_base + CallbackResult - CodeBase);
    const void* original = nullptr;
    const Hooks::Result result = Hooks::Replace(module_base, CasterItem, c, capacity,
                                                reinterpret_cast<const void*>(&OnItem), &original);
    if (result != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: the caster item callback at {:#x} is not replaced ({})",
                  CasterItem.address, static_cast<u32>(result));
        return false;
    }
    g_draw.original = reinterpret_cast<ItemCallback>(const_cast<void*>(original));
    return true;
}

void Begin(u32 cascade, u32 frame) {
    DrawThread& t = g_draw;
    t.active = t.original != nullptr && g_enabled.load(std::memory_order_relaxed);
    t.tcb = Core::GetTcbBase();
    t.compare = frame % CompareInterval == CompareInterval / 2;
    t.started = false;
    t.cascade = cascade;
    t.frame = frame;
    t.first = t.written;
    t.fallbacks = 0;
}

void End() {
    DrawThread& t = g_draw;
    if (!t.active) {
        return;
    }
    t.active = false;
    if (!t.started) {
        return;
    }
    const u32 count = t.written - t.first;
    Marker::Write({Marker::Kind::CasterBatch, ShadowPass::FirstPass + t.cascade, t.frame,
                   t.fallbacks, 0, t.first, count, t.compare ? Marker::CompareBatch : 0u});
    g_batched.fetch_add(count, std::memory_order_relaxed);
}

void Enable() {
    g_enabled.store(true, std::memory_order_relaxed);
}

void Disable() {
    g_enabled.store(false, std::memory_order_relaxed);
}

const Record* Ring() {
    return g_ring.data();
}

void Release(u32 end) {
    g_released.store(end, std::memory_order_release);
}

Counters Drain() {
    return {g_batched.exchange(0, std::memory_order_relaxed),
            g_unsupported.exchange(0, std::memory_order_relaxed),
            g_ring_full.exchange(0, std::memory_order_relaxed)};
}

} // namespace Gr2Pt::CasterBatch
