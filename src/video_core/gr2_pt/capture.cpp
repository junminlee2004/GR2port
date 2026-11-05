// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

#include "video_core/gr2_pt/capture.h"
#include "video_core/gr2_pt/meshes.h"
#include "video_core/gr2_pt/scene.h"
#include "video_core/gr2_pt/scene_ring.h"
#include "video_core/gr2_pt/textures.h"

namespace Gr2Pt::Capture {

namespace {

// View: item pool base, sort key count, key array end, per-category counts and first keys, and
// the list of BG_Main items drawn instanced.
constexpr u64 ViewItems = 0xb60;
constexpr u64 ViewKeyCount = 0xb58;
constexpr u64 ViewKeys = 0xb70;
constexpr u64 ViewCategoryCount = 0xecc;
constexpr u64 ViewCategoryFirst = 0xe34;
constexpr u64 ViewBatchList = 0xb80;
// Item: submesh +0x08, vertex buffer object +0x10, skin vertex buffer object +0x18, index
// buffer object +0x20 (at shape + 0x188).
constexpr u64 ItemSubmesh = 0x08;
constexpr u64 ItemVertexBuffer = 0x10;
constexpr u64 ItemSkin = 0x18;
constexpr u64 ItemIndexBuffer = 0x20;
constexpr u64 ShapeVertexBuffer = 0x60;
constexpr u64 ShapeIndexBuffer = 0x188;
// Submesh: first index in the shape's indices +0x20, range count +0x1e, first range +0x40.
constexpr u64 SubmeshFirstIndex = 0x20;
constexpr u64 SubmeshRanges = 0x1e;
constexpr u64 SubmeshRange = 0x40;

// The opaque mesh categories. Each item holds its vertex constants, which start with WorldView
// and WorldViewProj, its colour texture object and its pixel constants; the pixel constants hold
// the colour scale and the colour texture coordinate transform (offset xy, repeat zw).
constexpr u64 None = ~0ull;
struct Category {
    u32 index;
    u64 constants;
    u64 texture;
    u64 material;
    u64 color_scale;
    u64 texcoord_transform;
};
constexpr std::array<Category, 8> Categories{{
    {15, 0x60, 0x38, 0x68, 0x00, 0x20}, // G2_BG_Main
    {16, 0x38, 0x48, 0x40, 0x80, 0x00}, // BG layer: the first of four layers
    {17, 0x38, 0x48, 0x40, 0x00, None}, // BG single
    {19, 0x38, 0x48, 0x40, 0x00, None}, // BG gem
    {21, 0x38, 0x48, 0x40, 0x00, None}, // BG interior
    {22, 0x38, 0x48, 0x40, 0x30, 0x00}, // BG vein
    {23, 0x30, None, None, None, None}, // Far: untextured
    {8, 0x68, 0x30, 0x70, 0xa0, None},  // G2_CH_Main: the colour texture is t1
}};
constexpr std::array<float, 4> NoScale{1.0f, 1.0f, 1.0f, 1.0f};
constexpr std::array<float, 4> NoTransform{0.0f, 0.0f, 1.0f, 1.0f};
constexpr u32 BatchedCategory = 15;
constexpr u64 MatrixBytes = 64;

std::atomic<u64> g_frames{0};

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// Writes the draws of a packet. The packet's memory is write-combined, where reads are slow:
// the writer only stores to it, and counts in its own members.
class Writer {
public:
    explicit Writer(u8* packet)
        : draws{reinterpret_cast<Scene::Draw*>(packet + Scene::DrawsOffset)},
          commands{reinterpret_cast<Scene::Command*>(packet + Scene::CommandsOffset)} {}

    const Scene::Header& Header() const {
        return header;
    }

    void Add(VAddr item, const Category& category) {
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr constants = Load<VAddr>(item + category.constants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || constants == 0 ||
            Load<VAddr>(item + ItemSkin) != 0 ||
            Load<VAddr>(item + ItemVertexBuffer) != shape + ShapeVertexBuffer ||
            Load<u16>(submesh + SubmeshRanges) == 0 || header.draws == Scene::MaxDraws) {
            header.dropped++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr) {
            header.dropped++;
            return;
        }
        const u32 first =
            Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
        const u32 count = Load<u32>(submesh + SubmeshRange + 4);
        if (u64{first} + count > mesh->indices) {
            header.dropped++;
            return;
        }
        header.indices += count;
        header.max_indices = std::max(header.max_indices, count);
        u32 texture = 0;
        if (category.texture != None) {
            const VAddr object = Load<VAddr>(item + category.texture);
            if (object != 0) {
                texture = Textures::Get(object);
                header.untextured += texture == 0;
            }
        }
        const VAddr material =
            category.material != None ? Load<VAddr>(item + category.material) : 0;
        const void* color_scale = NoScale.data();
        const void* texcoord_transform = NoTransform.data();
        if (material != 0 && category.color_scale != None) {
            color_scale = reinterpret_cast<const void*>(material + category.color_scale);
        }
        if (material != 0 && category.texcoord_transform != None) {
            texcoord_transform =
                reinterpret_cast<const void*>(material + category.texcoord_transform);
        }
        const u32 i = header.draws++;
        // The fields are written in order: the packet is write-combined.
        Scene::Draw& draw = draws[i];
        std::memcpy(draw.world_view, reinterpret_cast<const void*>(constants), 2 * MatrixBytes);
        std::memcpy(draw.color_scale, color_scale, sizeof(draw.color_scale));
        std::memcpy(draw.texcoord_transform, texcoord_transform, sizeof(draw.texcoord_transform));
        draw.vertices = mesh->vertices;
        draw.stride = mesh->stride;
        draw.position = mesh->position;
        draw.texcoord = mesh->texcoord;
        draw.texture = texture;
        commands[i] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = 0,
        };
    }

private:
    Scene::Header header{};
    Scene::Draw* draws;
    Scene::Command* commands;
};

} // namespace

void OnFrame(VAddr view) {
    const auto start = std::chrono::steady_clock::now();
    const u64 frame = g_frames.load(std::memory_order_relaxed) + 1;
    g_frames.store(frame, std::memory_order_relaxed);
    u8* const packet = SceneRing::Begin();
    if (packet == nullptr) {
        return;
    }
    Writer writer{packet};
    const VAddr items = Load<VAddr>(view + ViewItems);
    const VAddr keys = Load<VAddr>(view + ViewKeys);
    const s32 key_count = Load<s32>(view + ViewKeyCount);
    for (const Category& category : Categories) {
        const s32 count = Load<s32>(view + ViewCategoryCount + category.index * 4);
        const s32 first = Load<s32>(view + ViewCategoryFirst + category.index * 4);
        const VAddr key = keys + static_cast<s64>(first - key_count) * 4;
        for (s32 i = 0; i < count && i < static_cast<s32>(Scene::MaxDraws); i++) {
            const u32 index = Load<u32>(key + i * 4) & 0x3fffff;
            writer.Add(items + (static_cast<u64>(index) << 6), category);
        }
    }
    const VAddr batch = Load<VAddr>(view + ViewBatchList);
    if (batch != 0) {
        const VAddr entries = Load<VAddr>(batch);
        const s32 count = Load<s32>(batch + 8);
        for (s32 i = 0; i < count && i < static_cast<s32>(Scene::MaxDraws); i++) {
            const u32 index = Load<u32>(entries + i * 8 + 4);
            writer.Add(items + static_cast<u32>(index << 6), Categories[0]);
        }
    }
    Scene::Header header = writer.Header();
    header.frame = frame;
    header.capture_ns = static_cast<u32>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    std::memcpy(packet, &header, sizeof(header));
    SceneRing::Publish(frame);
}

u64 Frames() {
    return g_frames.load(std::memory_order_relaxed);
}

} // namespace Gr2Pt::Capture
