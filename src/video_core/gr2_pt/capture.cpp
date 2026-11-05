// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
// View: V +0x20, inverse(V) +0x60, the cascade records from +0x400 every 0x110 ({valid, split,
// depth bias constant, depth bias slope} and the cascade VP at +0x90), and the casters'
// view-projection tables from +0x748 (a pointer per table and cascade).
constexpr u64 ViewMatrix = 0x20;
constexpr u64 ViewInverse = 0x60;
constexpr u64 ViewCascades = 0x400;
constexpr u64 CascadeBytes = 0x110;
constexpr u64 CascadeBiasConstant = 0x08;
constexpr u64 CascadeBiasSlope = 0x0c;
constexpr u64 CascadeViewProjection = 0x90;
constexpr u64 ViewCasterTables = 0x748;
// View: the key light's colour times intensity +0x7b0 and the view-space direction its light
// travels in +0x7c0.
constexpr u64 ViewKeyLight = 0x7b0;
constexpr u64 CasterTableBytes = 0x18;
// Shadow caster items: flags +4 (0x08 culls back faces, 0x10 and 0x20 pick a view-projection
// table), cascade mask +5, world matrix +0x30. Alpha-tested casters hold pixel constants {alpha
// reference or -1, fade or -1} and a colour texture object. Each category keeps an index range
// that overrides the submesh's.
constexpr u64 CasterFlags = 0x4;
constexpr u64 CasterMask = 0x5;
constexpr u64 CasterWorld = 0x30;
// A field a category does not have.
constexpr u64 None = ~0ull;
struct CasterCategory {
    u32 index;
    u64 range;
    u64 constants;
    u64 texture;
};
constexpr std::array<CasterCategory, 2> CasterCategories{{
    {0, 0x38, None, None}, // FUN_0141c510
    {2, 0x48, 0x38, 0x40}, // FUN_0141c9b0: alpha-tested
}};
// Item: bone count +0x06, submesh +0x08, vertex buffer object +0x10 (the shape's, or a deformed
// copy the game's compute writes each frame), skin vertex buffer object +0x18 (in the shape's
// skin object), index buffer object +0x20 (at shape + 0x188), bone palette +0x28 (3x4 matrices).
constexpr u64 ItemBones = 0x06;
constexpr u64 ItemSubmesh = 0x08;
constexpr u64 ItemVertexBuffer = 0x10;
constexpr u64 ItemSkin = 0x18;
constexpr u64 ItemIndexBuffer = 0x20;
constexpr u64 ItemPalette = 0x28;
constexpr u64 ShapeVertexBuffer = 0x60;
constexpr u64 ShapeIndexBuffer = 0x188;
constexpr u64 ShapeSkin = 0x1a8;
constexpr u64 SkinVertexBuffer = 0x68;
constexpr u64 BoneBytes = 48;
// Submesh: first index in the shape's indices +0x20, range count +0x1e, first range +0x40.
constexpr u64 SubmeshFirstIndex = 0x20;
constexpr u64 SubmeshRanges = 0x1e;
constexpr u64 SubmeshRange = 0x40;

// The opaque mesh categories. Each item holds its vertex constants, which start with WorldView
// and WorldViewProj, its colour texture object and its pixel constants; the pixel constants hold
// the colour scale and the colour texture coordinate transform (offset xy, repeat zw).
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
    /// packet is at address on the GPU.
    Writer(u8* packet, u64 address)
        : draws{reinterpret_cast<Scene::Draw*>(packet + Scene::DrawsOffset)},
          commands{reinterpret_cast<Scene::Command*>(packet + Scene::CommandsOffset)},
          casters{reinterpret_cast<Scene::Caster*>(packet + Scene::CastersOffset)},
          caster_commands{reinterpret_cast<Scene::Command*>(packet + Scene::CasterCommandsOffset)},
          palettes{packet + Scene::PalettesOffset},
          palettes_address{address + Scene::PalettesOffset} {}

    void SetCascades(u32 mask) {
        header.cascades = mask;
    }

    void SetDepthBias(u32 cascade, float constant, float slope) {
        header.depth_bias[cascade][0] = constant;
        header.depth_bias[cascade][1] = slope;
    }

    const Scene::Header& Header() const {
        return header;
    }

    void Add(VAddr item, const Category& category) {
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr constants = Load<VAddr>(item + category.constants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || constants == 0 ||
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
        Skin skin;
        if (u64{first} + count > mesh->indices || !ReadSkin(item, shape, *mesh, skin)) {
            header.dropped++;
            return;
        }
        header.indices += count;
        header.skinned += skin.bones != 0;
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
        draw.skin = skin.stream;
        draw.palette = skin.palette;
        draw.bones = skin.bones;
        commands[i] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = 0,
        };
    }

    // The game draws a caster in each cascade its mask names, with the index range of the
    // category's override when there is one.
    void AddCaster(VAddr item, const CasterCategory& category) {
        const u32 mask = Load<u8>(item + CasterMask) & header.cascades;
        if (mask == 0) {
            return;
        }
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr world = Load<VAddr>(item + CasterWorld);
        const VAddr range = Load<VAddr>(item + category.range);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || world == 0 ||
            Load<VAddr>(item + ItemVertexBuffer) != shape + ShapeVertexBuffer ||
            header.casters == Scene::MaxCasters) {
            header.dropped_casters++;
            return;
        }
        u32 first = 0;
        u32 count = 0;
        if (range == 0) {
            if (Load<u16>(submesh + SubmeshRanges) == 0) {
                return;
            }
            first = Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
            count = Load<u32>(submesh + SubmeshRange + 4);
        } else {
            first = Load<u32>(submesh + SubmeshFirstIndex);
            count = Load<u32>(range + SubmeshRange) - (Load<u32>(submesh + SubmeshRange) + first) +
                    Load<u32>(range + SubmeshRange + 4) + Load<u32>(range + SubmeshFirstIndex);
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr || count == 0 || u64{first} + count > mesh->indices) {
            header.dropped_casters++;
            return;
        }
        Skin skin;
        if (!ReadSkin(item, shape, *mesh, skin)) {
            header.dropped_casters++;
            return;
        }
        header.skinned_casters += skin.bones != 0;
        const u8 flags = Load<u8>(item + CasterFlags);
        const u32 table = (flags & 0x10) != 0 ? 1 : (flags & 0x20) != 0 ? 2 : 0;
        // A reference below 0 passes every texel: the caster is opaque.
        float alpha_reference = -1.0f;
        u32 texture = 0;
        if (category.constants != None && mesh->texcoord != 0) {
            const VAddr constants = Load<VAddr>(item + category.constants);
            const VAddr object = Load<VAddr>(item + category.texture);
            if (constants != 0 && object != 0) {
                alpha_reference = Load<float>(constants);
                texture = Textures::Get(object);
            }
        }
        const bool alpha_tested = alpha_reference >= 0.0f && texture != 0;
        const u32 list = (alpha_tested ? 2 : 0) + ((flags & 0x08) != 0 ? 0 : 1);
        const u32 i = header.casters++;
        Scene::Caster& caster = casters[i];
        std::memcpy(caster.world, reinterpret_cast<const void*>(world), MatrixBytes);
        caster.vertices = mesh->vertices;
        caster.stride = mesh->stride;
        caster.position = mesh->position;
        caster.table = table;
        caster.texture = texture;
        caster.texcoord = mesh->texcoord;
        caster.alpha_reference = alpha_reference;
        caster.skin = skin.stream;
        caster.palette = skin.palette;
        caster.bones = skin.bones;
        for (u32 c = 0; c < Scene::Cascades; c++) {
            if ((mask >> c & 1) == 0) {
                continue;
            }
            header.caster_indices[c] += count;
            u32& n = header.caster_commands[c][list];
            caster_commands[(c * Scene::CasterLists + list) * Scene::MaxCasters + n++] = {
                .index_count = count,
                .instance_count = 1,
                .first_index = mesh->first_index + first,
                .vertex_offset = 0,
                .first_instance = i,
            };
        }
    }

private:
    struct Skin {
        u64 stream = 0;
        u64 palette = 0;
        u32 bones = 0;
    };

    // Copies a palette into the packet once for each run of users that share it.
    bool CopyPalette(VAddr palette, u32 bones, u64& address) {
        if (palette != last_palette || bones != last_bones) {
            const u32 rows = bones * 3;
            if (header.palette_rows + rows > Scene::MaxPaletteRows) {
                return false;
            }
            std::memcpy(palettes + u64{header.palette_rows} * 16,
                        reinterpret_cast<const void*>(palette), bones * BoneBytes);
            last_palette = palette;
            last_bones = bones;
            last_address = palettes_address + u64{header.palette_rows} * 16;
            header.palette_rows += rows;
        }
        address = last_address;
        return true;
    }

    // The skin of an item whose shape is skinned, with the item's palette in the packet. False
    // when the item cannot be drawn.
    bool ReadSkin(VAddr item, VAddr shape, const Meshes::Mesh& mesh, Skin& skin) {
        const VAddr stream = Load<VAddr>(item + ItemSkin);
        if (mesh.skin == 0 && stream == 0) {
            skin = {};
            return true;
        }
        const u32 bones = Load<u16>(item + ItemBones);
        const VAddr palette = Load<VAddr>(item + ItemPalette);
        if (mesh.skin == 0 || stream != Load<VAddr>(shape + ShapeSkin) + SkinVertexBuffer ||
            bones == 0 || palette == 0) {
            return false;
        }
        u64 address = 0;
        if (!CopyPalette(palette, bones, address)) {
            return false;
        }
        skin = {.stream = mesh.skin, .palette = address, .bones = bones};
        return true;
    }

    Scene::Header header{};
    Scene::Draw* draws;
    Scene::Command* commands;
    Scene::Caster* casters;
    Scene::Command* caster_commands;
    u8* palettes;
    u64 palettes_address;
    VAddr last_palette = 0;
    u32 last_bones = 0;
    u64 last_address = 0;
};

// Items are scattered through the pool and point at scattered objects. The loops prefetch the
// item Lookahead places ahead, and the objects of the item half as far ahead.
constexpr s32 Lookahead = 8;

void Prefetch(VAddr address) {
    __builtin_prefetch(reinterpret_cast<const void*>(address));
}

// The submesh, the shape's handle (just before its index buffer object) and the matrices of an
// item.
void PrefetchObjects(VAddr item, u64 matrices) {
    const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
    Prefetch(submesh);
    Prefetch(submesh + SubmeshRange);
    Prefetch(Load<VAddr>(item + ItemIndexBuffer) - 4);
    Prefetch(Load<VAddr>(item + matrices));
}

template <typename ItemAt, typename Visit>
void ForEachItem(s32 count, u64 matrices, ItemAt item_at, Visit visit) {
    for (s32 i = 0; i < count; i++) {
        if (i + Lookahead < count) {
            Prefetch(item_at(i + Lookahead));
        }
        if (i + Lookahead / 2 < count) {
            PrefetchObjects(item_at(i + Lookahead / 2), matrices);
        }
        visit(item_at(i));
    }
}

// Writes the cascades into the packet's shadow block and the header's cascade fields.
void WriteShadows(VAddr view, u8* packet, Writer& writer) {
    auto* shadows = reinterpret_cast<Scene::Shadows*>(packet + Scene::ShadowsOffset);
    std::memcpy(shadows->inverse_view, reinterpret_cast<const void*>(view + ViewInverse),
                MatrixBytes);
    u32 mask = 0;
    for (u32 c = 0; c < Scene::Cascades; c++) {
        const VAddr cascade = view + ViewCascades + c * CascadeBytes;
        mask |= (Load<u32>(cascade) != 0 ? 1u : 0u) << c;
        writer.SetDepthBias(c, Load<float>(cascade + CascadeBiasConstant),
                            Load<float>(cascade + CascadeBiasSlope));
        for (u32 table = 0; table < Scene::CasterTables; table++) {
            VAddr matrix = Load<VAddr>(view + ViewCasterTables + table * CasterTableBytes + c * 8);
            if (matrix == 0) {
                matrix = cascade + CascadeViewProjection;
            }
            std::memcpy(shadows->view_projection[c * Scene::CasterTables + table],
                        reinterpret_cast<const void*>(matrix), MatrixBytes);
        }
    }
    writer.SetCascades(mask);
    // Clip depth grows away from the light, so the first cascade's depth row, negated, points
    // toward it. Row-vector matrices: a direction d maps to d * M.
    const auto vp = Load<std::array<float, 16>>(view + ViewCascades + CascadeViewProjection);
    const auto v = Load<std::array<float, 16>>(view + ViewMatrix);
    const std::array<float, 3> toward{-vp[2], -vp[6], -vp[10]};
    std::array<float, 4> direction{};
    for (u32 j = 0; j < 3; j++) {
        direction[j] = toward[0] * v[j] + toward[1] * v[4 + j] + toward[2] * v[8 + j];
    }
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                   direction[2] * direction[2]);
    if (length > 0.0f) {
        for (u32 j = 0; j < 3; j++) {
            direction[j] /= length;
        }
    }
    std::memcpy(shadows->light_direction, direction.data(), sizeof(direction));
    std::memcpy(shadows->key_light_color, reinterpret_cast<const void*>(view + ViewKeyLight),
                sizeof(shadows->key_light_color) + sizeof(shadows->key_light_direction));
}

} // namespace

void OnFrame(VAddr view) {
    const auto start = std::chrono::steady_clock::now();
    const u64 frame = g_frames.load(std::memory_order_relaxed) + 1;
    g_frames.store(frame, std::memory_order_relaxed);
    u8* const packet = SceneRing::Begin();
    if (packet == nullptr) {
        return;
    }
    Writer writer{packet, SceneRing::Address(packet)};
    WriteShadows(view, packet, writer);
    const VAddr items = Load<VAddr>(view + ViewItems);
    const VAddr keys = Load<VAddr>(view + ViewKeys);
    const s32 key_count = Load<s32>(view + ViewKeyCount);
    const auto keyed = [items, keys, key_count, view](u32 category) {
        const s32 first = Load<s32>(view + ViewCategoryFirst + category * 4);
        const VAddr key = keys + static_cast<s64>(first - key_count) * 4;
        return [items, key](s32 i) {
            return items + (static_cast<u64>(Load<u32>(key + i * 4) & 0x3fffff) << 6);
        };
    };
    for (const CasterCategory& category : CasterCategories) {
        const s32 count = Load<s32>(view + ViewCategoryCount + category.index * 4);
        ForEachItem(count, CasterWorld, keyed(category.index),
                    [&](VAddr item) { writer.AddCaster(item, category); });
    }
    for (const Category& category : Categories) {
        const s32 count = std::min(Load<s32>(view + ViewCategoryCount + category.index * 4),
                                   static_cast<s32>(Scene::MaxDraws));
        ForEachItem(count, category.constants, keyed(category.index),
                    [&](VAddr item) { writer.Add(item, category); });
    }
    const VAddr batch = Load<VAddr>(view + ViewBatchList);
    if (batch != 0) {
        const VAddr entries = Load<VAddr>(batch);
        const s32 count = std::min(Load<s32>(batch + 8), static_cast<s32>(Scene::MaxDraws));
        ForEachItem(
            count, Categories[0].constants,
            [items, entries](s32 i) {
                return items + static_cast<u32>(Load<u32>(entries + i * 8 + 4) << 6);
            },
            [&](VAddr item) { writer.Add(item, Categories[0]); });
    }
    Scene::Header header = writer.Header();
    header.frame = frame;
    for (u32 c = 0; c < Scene::ItemCategories; c++) {
        const s32 count = Load<s32>(view + ViewCategoryCount + c * 4);
        header.category_items[c] = static_cast<u16>(std::clamp(count, 0, 0xffff));
    }
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
