// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/meshes.h"

namespace Gr2Port::Meshes {

namespace {

// The .gfx builder FUN_01447990 returns the model; the model unload FUN_01451980 takes a pointer
// to the model in rsi.
constexpr Hooks::Entry Builder{0x1447990, 6, Hooks::FramePrologue};
constexpr Hooks::Entry Unload{0x1451980, 6, Hooks::FramePrologue};

// Model: shape entries of 0x50 bytes at *(model + 0xb0) + *(model + 0xbc), count at +0xb8, the
// shape pointer first in each.
constexpr u64 ModelBlock = 0xb0;
constexpr u64 ModelShapeCount = 0xb8;
constexpr u64 ModelShapeOffset = 0xbc;
constexpr u64 ShapeEntrySize = 0x50;
// Shape: SObj kind at +0xa, vertex buffer object at +0x60, index buffer object at +0x188, skin
// object at +0x1a8.
constexpr u64 ShapeKind = 0xa;
constexpr u16 KindShape = 6;
constexpr u64 ShapeVertexBuffer = 0x60;
constexpr u64 ShapeHandle = 0x184;
constexpr u64 ShapeIndexBuffer = 0x188;
constexpr u64 ShapeSkin = 0x1a8;
constexpr u64 ShapeSoftBody = 0x1b0;
// SoftBody object: particle count +0x30, the buffer objects of the particle records +0x88, the
// constraints +0xa0 and the vertex to particle map +0xb8, each a MemBlock* then a V#.
constexpr u64 SoftBodyParticleCount = 0x30;
constexpr u64 SoftBodyParticles = 0x88;
constexpr u64 SoftBodyConstraints = 0xa0;
constexpr u64 SoftBodyMap = 0xb8;
constexpr u32 ParticleBytes = 8;
// Skin object: the vertex buffer object of the skin stream at +0x68.
constexpr u64 SkinVertexBuffer = 0x68;
constexpr u32 SkinStride = 16;
// Vertex buffer object: MemBlock* at +0, stride +0x18, element count +0x1c, vertex count +0x20,
// element of each semantic at +0x24, elements {format, stride, offset, semantic} at +0x34.
constexpr u64 VertexStride = 0x18;
constexpr u64 VertexElements = 0x1c;
constexpr u64 VertexSemanticElement = 0x24;
constexpr u64 VertexElementInfo = 0x34;
constexpr u32 SemanticPosition = 0;
constexpr u32 SemanticNormal = 2;
constexpr u32 SemanticVertexColor = 3;
constexpr u32 SemanticTangent = 14;
// Index buffer object: MemBlock* at +0, index size flag at +0x18 (0 = 16-bit).
constexpr u64 IndexSizeFlag = 0x18;
// MemBlock: data address at +0, size at +8.
constexpr u64 BlockSize = 8;

// Vertex formats the shaders decode: RGB32 FLOAT, RGBA32 FLOAT, RGBA16 FLOAT.
constexpr bool PositionFormat(u32 format) {
    return format == 3 || format == 4 || format == 7;
}

// Texture coordinate formats the shaders decode: RG32 FLOAT, RG16 FLOAT, RG16 SINT.
constexpr bool TexcoordFormat(u32 format) {
    return format == 2 || format == 6 || format == 12;
}

// The normal is 10_11_11 FLOAT, the tangent RGBA16 FLOAT.
constexpr bool NormalFormat(u32 format) {
    return format == 20;
}

constexpr bool TangentFormat(u32 format) {
    return format == 7;
}

// Vertex colour formats the shaders decode: RGBA8 UNORM, RGBA16 UNORM, RGBA16 FLOAT.
constexpr bool VertexColorFormat(u32 format) {
    return format == 16 || format == 10 || format == 7;
}

constexpr u64 UploadBudget = 64_MB;
constexpr u32 LoggedUploads = 8;
constexpr u32 LoggedSkins = 4;
constexpr u32 LoggedVertexStreams = 12;
constexpr u32 LoggedCloth = 4;
constexpr u64 IndexCapacity = 64ull << 20;

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

std::array<Mesh, Capacity> g_meshes;

// Guards the handle lists and the reads of game memory that unloads free.
std::mutex g_mutex;
std::vector<u32> g_free;
u32 g_unused = 0;
std::deque<u32> g_pending;
std::vector<u32> g_unloaded;

// Render thread only.
struct Resident {
    Buffer vertices;
    u32 first_index = 0;
    u32 indices = 0;
    bool skinned = false;
    bool cloth = false;
};
struct Retiring {
    u32 handle;
    // The last frame that can draw the mesh.
    u64 frame;
};
struct Staging {
    Buffer buffer;
    u64 frame;
};
const Device* g_device = nullptr;
Memory* g_memory = nullptr;
Buffer g_index_buffer;
std::map<u32, u32> g_free_indices;
std::array<Resident, Capacity> g_resident;
std::vector<Retiring> g_retiring;
std::vector<Staging> g_staging;
Stats g_stats{};

bool AllocateIndices(u32 count, u32& first) {
    if (count == 0) {
        first = 0;
        return true;
    }
    for (auto it = g_free_indices.begin(); it != g_free_indices.end(); ++it) {
        if (it->second < count) {
            continue;
        }
        first = it->first;
        const u32 left = it->second - count;
        g_free_indices.erase(it);
        if (left != 0) {
            g_free_indices.emplace(first + count, left);
        }
        return true;
    }
    return false;
}

void FreeIndices(u32 first, u32 count) {
    if (count == 0) {
        return;
    }
    auto [it, inserted] = g_free_indices.emplace(first, count);
    const auto next = std::next(it);
    if (next != g_free_indices.end() && it->first + it->second == next->first) {
        it->second += next->second;
        g_free_indices.erase(next);
    }
    if (it != g_free_indices.begin()) {
        const auto previous = std::prev(it);
        if (previous->first + previous->second == it->first) {
            previous->second += it->second;
            g_free_indices.erase(it);
        }
    }
}

template <typename F>
void ForEachShape(VAddr model, F&& f) {
    const VAddr block = Load<VAddr>(model + ModelBlock);
    const u32 count = Load<u32>(model + ModelShapeCount);
    const s32 offset = Load<s32>(model + ModelShapeOffset);
    if (block == 0) {
        return;
    }
    for (u32 i = 0; i < count; i++) {
        const VAddr shape = Load<VAddr>(block + offset + i * ShapeEntrySize);
        if (shape != 0 && Load<u16>(shape + ShapeKind) == KindShape) {
            f(shape);
        }
    }
}

u32 g_missed_unloads = 0;
u32 g_lost_handles = 0;

void PS4_SYSV_ABI OnBuilt(u32, const Hooks::Arguments*, u64 model) {
    if (model == 0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ForEachShape(model, [](VAddr shape) {
        // The handle field starts as heap contents, which can be the handle of an earlier
        // shape at this address.
        const u32 earlier = Load<u32>(shape + ShapeHandle);
        if (earlier < Capacity &&
            g_meshes[earlier].shape.load(std::memory_order_relaxed) == shape) {
            g_meshes[earlier].ready.store(false, std::memory_order_relaxed);
            g_meshes[earlier].shape.store(0, std::memory_order_release);
            g_unloaded.push_back(earlier);
            g_missed_unloads++;
        }
        u32 handle = Capacity;
        if (!g_free.empty()) {
            handle = g_free.back();
            g_free.pop_back();
        } else if (g_unused < Capacity) {
            handle = g_unused++;
        }
        std::memcpy(reinterpret_cast<void*>(shape + ShapeHandle), &handle, sizeof(handle));
        if (handle == Capacity) {
            return;
        }
        g_meshes[handle].ready.store(false, std::memory_order_relaxed);
        g_meshes[handle].shape.store(shape, std::memory_order_release);
        g_pending.push_back(handle);
    });
}

void PS4_SYSV_ABI OnUnload(u32, const Hooks::Registers* registers, u64) {
    const VAddr model = Load<VAddr>(registers->rsi);
    if (model == 0) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    ForEachShape(model, [](VAddr shape) {
        const u32 handle = Load<u32>(shape + ShapeHandle);
        if (handle >= Capacity || g_meshes[handle].shape.load(std::memory_order_relaxed) != shape) {
            g_lost_handles += handle != Capacity;
            return;
        }
        g_meshes[handle].ready.store(false, std::memory_order_relaxed);
        g_meshes[handle].shape.store(0, std::memory_order_release);
        g_unloaded.push_back(handle);
    });
}

struct Source {
    VAddr vertices;
    u32 vertex_bytes;
    VAddr indices;
    u32 index_bytes;
    u32 stride;
    u32 position;
    u32 texcoord;
    u32 texcoord1;
    u32 normal;
    u32 tangent;
    u32 vertex_color;
    VAddr skin = 0;
    u32 skin_bytes = 0;
    VAddr particles = 0;
    u32 particle_bytes = 0;
    VAddr constraints = 0;
    u32 constraint_bytes = 0;
    VAddr particle_map = 0;
    u32 map_bytes = 0;
    u32 particle_count = 0;
};

// The shape's cloth data, when it has some.
void ReadSoftBody(VAddr shape, Source& source) {
    const VAddr soft_body = Load<VAddr>(shape + ShapeSoftBody);
    if (soft_body == 0) {
        return;
    }
    const auto block = [soft_body](u64 object, VAddr& data, u32& bytes) {
        const VAddr memory = Load<VAddr>(soft_body + object);
        data = memory != 0 ? Load<VAddr>(memory) : 0;
        bytes = memory != 0 ? Load<u32>(memory + BlockSize) : 0;
    };
    block(SoftBodyParticles, source.particles, source.particle_bytes);
    block(SoftBodyConstraints, source.constraints, source.constraint_bytes);
    block(SoftBodyMap, source.particle_map, source.map_bytes);
    source.particle_count = Load<u32>(soft_body + SoftBodyParticleCount);
}

// Whether the shaders can run the cloth data without reading past it: every anchor names a
// vertex, every constraint range lies in the constraints, and every constraint and map entry names
// a particle. The game's copy is read once, at upload.
bool ValidSoftBody(const Source& source, u32 vertex_count) {
    const u32 count = source.particle_count;
    const u32 constraint_count = source.constraint_bytes / 4;
    if (source.particles == 0 || source.constraints == 0 || source.particle_map == 0 ||
        count == 0 || u64{count} * ParticleBytes > source.particle_bytes ||
        u64{vertex_count} * 2 > source.map_bytes) {
        return false;
    }
    const auto* particles = reinterpret_cast<const u32*>(source.particles);
    for (u32 i = 0; i < count; i++) {
        const u32 anchor = particles[2 * i] & 0xffff;
        const u32 first = particles[2 * i] >> 16;
        const u32 links = particles[2 * i + 1] & 0xffff;
        if (anchor >= vertex_count || first + links > constraint_count) {
            return false;
        }
    }
    const auto* constraints = reinterpret_cast<const u32*>(source.constraints);
    for (u32 i = 0; i < constraint_count; i++) {
        if ((constraints[i] & 0xffff) >= count) {
            return false;
        }
    }
    const auto* map = reinterpret_cast<const u16*>(source.particle_map);
    return std::all_of(map, map + vertex_count, [count](u16 p) { return p < count; });
}

std::string DescribeElements(VAddr vb) {
    std::string elements;
    const u32 count = std::min<u32>(Load<u16>(vb + VertexElements), 16);
    for (u32 i = 0; i < count; i++) {
        const VAddr info = vb + VertexElementInfo + 4 * i;
        elements += fmt::format(" (format {} offset {} semantic {})", Load<u8>(info),
                                Load<u8>(info + 2), Load<u8>(info + 3));
    }
    return elements;
}

// Logs the elements of the first skin streams.
void LogSkin(VAddr shape, VAddr vb) {
    static u32 logged = 0;
    if (logged == LoggedSkins) {
        return;
    }
    logged++;
    LOG_INFO(Render_Vulkan, "GR2port: skin of mesh {:#x}: stride {},{}", shape,
             Load<u16>(vb + VertexStride), DescribeElements(vb));
}

// Logs the elements of the first vertex streams.
void LogVertices(VAddr shape, VAddr vb) {
    static u32 logged = 0;
    if (logged == LoggedVertexStreams) {
        return;
    }
    logged++;
    LOG_INFO(Render_Vulkan, "GR2port: vertices of mesh {:#x}: stride {},{}", shape,
             Load<u16>(vb + VertexStride), DescribeElements(vb));
}

// The element of a semantic, encoded as a position, when its format is one the shaders decode;
// else 0.
template <typename Accepted>
u32 ReadElement(VAddr vb, u32 semantic, Accepted accepted) {
    const u32 element = Load<u8>(vb + VertexSemanticElement + semantic);
    if (element >= Load<u16>(vb + VertexElements)) {
        return 0;
    }
    const u32 format = Load<u8>(vb + VertexElementInfo + 4 * element);
    const u32 offset = Load<u8>(vb + VertexElementInfo + 4 * element + 2);
    return accepted(format) && offset % 4 == 0 ? offset | format << 8 : 0;
}

// The texture coordinates: the two-component element of the lowest semantic after the position.
// Semantics follow each shader's inputs, so texture coordinates have no fixed one. skip passes
// over that many such elements.
u32 ReadTexcoord(VAddr vb, u32 skip = 0) {
    const u32 elements = std::min<u32>(Load<u16>(vb + VertexElements), 16);
    u32 texcoord = 0;
    u32 above = SemanticPosition;
    for (u32 pass = 0; pass <= skip; pass++) {
        texcoord = 0;
        u32 semantic = 16;
        for (u32 i = 0; i < elements; i++) {
            const VAddr info = vb + VertexElementInfo + 4 * i;
            const u32 format = Load<u8>(info);
            const u32 offset = Load<u8>(info + 2);
            const u32 element_semantic = Load<u8>(info + 3);
            if (element_semantic > above && element_semantic < semantic && TexcoordFormat(format) &&
                offset % 4 == 0) {
                texcoord = offset | format << 8;
                semantic = element_semantic;
            }
        }
        above = semantic;
    }
    return texcoord;
}

// The game's vertex format table: RGBA16 FLOAT.
constexpr u32 FormatRgba16Float = 7;

// A half float's value.
float FromHalf(u32 half) {
    const u32 sign = (half & 0x8000) << 16;
    const u32 exponent = half >> 10 & 0x1f;
    const u32 fraction = half & 0x3ff;
    if (exponent == 0) {
        // Zero or a subnormal.
        const float value = static_cast<float>(fraction) * 0x1p-24f;
        return sign != 0 ? -value : value;
    }
    const u32 wide = exponent == 0x1f ? 0xff : exponent + 112;
    return std::bit_cast<float>(sign | wide << 23 | fraction << 13);
}

// Sets mesh's box around the positions LoadPosition of vertex.glsl reads from the shape's vertex
// data, which every index names. A NaN is no bound of a float position and, with a half
// position, leaves the box without that bound.
void SetBox(const Source& source, u32 vertex_count, Mesh& mesh) {
    constexpr float Unbounded = std::numeric_limits<float>::infinity();
    const u32 offset = source.position & 0xff;
    const bool halves = (source.position >> 8) == FormatRgba16Float;
    std::fill_n(mesh.box_low, 3, -Unbounded);
    std::fill_n(mesh.box_high, 3, Unbounded);
    if (vertex_count == 0 || offset + (halves ? 6 : 12) > source.stride) {
        return;
    }
    const u8* vertex = reinterpret_cast<const u8*>(source.vertices) + offset;
    if (halves) {
        // Halves order as their sign and magnitude do.
        std::array<s32, 3> low;
        std::array<s32, 3> high;
        low.fill(std::numeric_limits<s32>::max());
        high.fill(std::numeric_limits<s32>::min());
        for (u32 i = 0; i < vertex_count; i++, vertex += source.stride) {
            std::array<u16, 3> position;
            std::memcpy(position.data(), vertex, sizeof(position));
            for (u32 k = 0; k < 3; k++) {
                const s32 magnitude = position[k] & 0x7fff;
                const s32 key = (position[k] & 0x8000) != 0 ? -magnitude : magnitude;
                low[k] = std::min(low[k], key);
                high[k] = std::max(high[k], key);
            }
        }
        const auto value = [](s32 key) {
            return FromHalf(key < 0 ? 0x8000u | static_cast<u32>(-key) : static_cast<u32>(key));
        };
        for (u32 k = 0; k < 3; k++) {
            mesh.box_low[k] = value(low[k]);
            mesh.box_high[k] = value(high[k]);
        }
        return;
    }
    std::array<float, 3> low;
    std::array<float, 3> high;
    low.fill(Unbounded);
    high.fill(-Unbounded);
    for (u32 i = 0; i < vertex_count; i++, vertex += source.stride) {
        std::array<float, 3> position;
        std::memcpy(position.data(), vertex, sizeof(position));
        for (u32 k = 0; k < 3; k++) {
            low[k] = position[k] < low[k] ? position[k] : low[k];
            high[k] = position[k] > high[k] ? position[k] : high[k];
        }
    }
    std::copy(low.begin(), low.end(), mesh.box_low);
    std::copy(high.begin(), high.end(), mesh.box_high);
}

// Reads a shape's streams; false when the shaders cannot draw it. A shape without indices, as the
// game's Branch shapes, which it draws as patches of their vertices, gets no indices.
bool ReadShape(VAddr shape, Source& source) {
    const VAddr vb = shape + ShapeVertexBuffer;
    const VAddr vb_block = Load<VAddr>(vb);
    const VAddr ib = shape + ShapeIndexBuffer;
    const VAddr ib_block = Load<VAddr>(ib);
    const bool indexed = ib_block != 0 && Load<VAddr>(ib_block) != 0;
    if (vb_block == 0 || (indexed && Load<u32>(ib + IndexSizeFlag) != 0)) {
        return false;
    }
    const u32 element = Load<u8>(vb + VertexSemanticElement + SemanticPosition);
    if (element >= Load<u16>(vb + VertexElements)) {
        return false;
    }
    const u32 format = Load<u8>(vb + VertexElementInfo + 4 * element);
    const u32 offset = Load<u8>(vb + VertexElementInfo + 4 * element + 2);
    source = {
        .vertices = Load<VAddr>(vb_block),
        .vertex_bytes = Load<u32>(vb_block + BlockSize),
        .indices = indexed ? Load<VAddr>(ib_block) : 0,
        .index_bytes = indexed ? Load<u32>(ib_block + BlockSize) : 0,
        .stride = Load<u16>(vb + VertexStride),
        .position = offset | format << 8,
        .texcoord = ReadTexcoord(vb),
        .texcoord1 = ReadTexcoord(vb, 1),
        .normal = ReadElement(vb, SemanticNormal, NormalFormat),
        .tangent = ReadElement(vb, SemanticTangent, TangentFormat),
        .vertex_color = ReadElement(vb, SemanticVertexColor, VertexColorFormat),
    };
    LogVertices(shape, vb);
    const VAddr skin = Load<VAddr>(shape + ShapeSkin);
    if (skin != 0) {
        const VAddr skin_vb = skin + SkinVertexBuffer;
        const VAddr skin_block = Load<VAddr>(skin_vb);
        LogSkin(shape, skin_vb);
        if (skin_block == 0 || Load<u16>(skin_vb + VertexStride) != SkinStride) {
            return false;
        }
        source.skin = Load<VAddr>(skin_block);
        source.skin_bytes = Load<u32>(skin_block + BlockSize);
        if (source.skin == 0) {
            return false;
        }
    }
    ReadSoftBody(shape, source);
    return source.vertices != 0 && source.vertex_bytes != 0 &&
           (!indexed || source.index_bytes >= 2) && PositionFormat(format) &&
           source.stride % 4 == 0 && offset % 4 == 0;
}

void Retire(u64 frame, u64 completed) {
    std::vector<u32> unloaded;
    {
        std::scoped_lock lock{g_mutex};
        unloaded.swap(g_unloaded);
    }
    // The packet of this frame, or the one the capture was writing at the unload, which the next
    // frame takes at the latest, can still draw the mesh.
    for (const u32 handle : unloaded) {
        g_retiring.push_back({handle, frame + 1});
    }
    std::erase_if(g_retiring, [completed](const Retiring& r) {
        if (completed < r.frame) {
            return false;
        }
        Resident& resident = g_resident[r.handle];
        if (resident.vertices.allocation != nullptr) {
            g_stats.resident_bytes -= resident.vertices.size + 2ull * resident.indices;
            g_memory->Destroy(resident.vertices);
            FreeIndices(resident.first_index, resident.indices);
            g_stats.meshes--;
            g_stats.skinned -= resident.skinned;
            g_stats.cloth -= resident.cloth;
        }
        resident = {};
        std::scoped_lock lock{g_mutex};
        g_free.push_back(r.handle);
        return true;
    });
    std::erase_if(g_staging, [completed](Staging& s) {
        if (s.frame > completed) {
            return false;
        }
        g_memory->Destroy(s.buffer);
        return true;
    });
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    const Hooks::Result built =
        Hooks::ObserveReturn(module_base, Builder, c, capacity, &OnBuilt, 0);
    const Hooks::Result unload =
        Hooks::ObserveEntry(module_base, Unload, c, capacity, &OnUnload, 0);
    const auto ok = [](Hooks::Result r) {
        return r == Hooks::Result::Hooked || r == Hooks::Result::Chained;
    };
    if (!ok(built) || !ok(unload)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the mesh builder or unload is not the expected code");
        return false;
    }
    return true;
}

void CopyVertices(vk::CommandBuffer cmd, const Mesh& mesh, vk::Buffer destination, u64 offset) {
    const Resident& resident = g_resident[&mesh - g_meshes.data()];
    const vk::BufferCopy copy{.dstOffset = offset, .size = mesh.vertex_bytes};
    cmd.copyBuffer(resident.vertices.buffer, destination, copy, g_device->dispatch);
}

const Mesh* Get(VAddr shape) {
    const u32 handle = Load<u32>(shape + ShapeHandle);
    if (handle >= Capacity) {
        return nullptr;
    }
    const Mesh& mesh = g_meshes[handle];
    if (!mesh.ready.load(std::memory_order_acquire) ||
        mesh.shape.load(std::memory_order_relaxed) != shape) {
        return nullptr;
    }
    return &mesh;
}

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    g_index_buffer = memory.CreateBuffer(IndexCapacity * 2,
                                         vk::BufferUsageFlagBits::eIndexBuffer |
                                             vk::BufferUsageFlagBits::eTransferDst,
                                         Placement::Device);
    if (!g_index_buffer.buffer) {
        return false;
    }
    g_free_indices.emplace(0u, static_cast<u32>(IndexCapacity));
    return true;
}

void Update(vk::CommandBuffer cmd, u64 frame, u64 completed) {
    Retire(frame, completed);

    std::vector<u32> uploaded;
    u64 budget = UploadBudget;
    {
        std::scoped_lock lock{g_mutex};
        while (!g_pending.empty() && budget != 0) {
            const u32 handle = g_pending.front();
            g_pending.pop_front();
            Mesh& mesh = g_meshes[handle];
            const VAddr shape = mesh.shape.load(std::memory_order_relaxed);
            // A handle freed and reused before its first upload is queued twice.
            if (shape == 0 || g_resident[handle].vertices.buffer) {
                continue;
            }
            Source source;
            u32 first_index = 0;
            if (!ReadShape(shape, source) ||
                !AllocateIndices(source.index_bytes / 2, first_index)) {
                g_stats.skipped++;
                continue;
            }
            // The skin stream, then the cloth data, follow the vertex data in the mesh's buffer.
            const u32 vertex_count = source.vertex_bytes / std::max(source.stride, 1u);
            const bool cloth = source.particles != 0 && source.stride != 0;
            const bool valid_cloth = cloth && ValidSoftBody(source, vertex_count);
            g_stats.bad_cloth += cloth && !valid_cloth;
            const auto align = [](u32 offset) { return (offset + 15) & ~15u; };
            const u32 skin_offset = align(source.vertex_bytes);
            const u32 skin_end =
                source.skin != 0 ? skin_offset + source.skin_bytes : source.vertex_bytes;
            const u32 particles_offset = align(skin_end);
            const u32 constraints_offset = align(particles_offset + source.particle_bytes);
            const u32 map_offset = align(constraints_offset + source.constraint_bytes);
            // The shaders read the map as u32s.
            const u32 vertex_region =
                valid_cloth ? map_offset + ((source.map_bytes + 3) & ~3u) : skin_end;
            const u64 bytes = u64{vertex_region} + source.index_bytes;
            if (!g_memory->Reserve(vertex_region)) {
                FreeIndices(first_index, source.index_bytes / 2);
                g_pending.push_front(handle);
                break;
            }
            Staging staging{
                .buffer = g_memory->CreateBuffer(bytes, vk::BufferUsageFlagBits::eTransferSrc,
                                                 Placement::Staging),
                .frame = frame,
            };
            Buffer vertices = g_memory->CreateBuffer(vertex_region,
                                                     vk::BufferUsageFlagBits::eStorageBuffer |
                                                         vk::BufferUsageFlagBits::eTransferSrc |
                                                         vk::BufferUsageFlagBits::eTransferDst,
                                                     Placement::Device);
            if (staging.buffer.mapped == nullptr || !vertices.buffer) {
                FreeIndices(first_index, source.index_bytes / 2);
                g_memory->Destroy(staging.buffer);
                g_memory->Destroy(vertices);
                g_stats.skipped++;
                continue;
            }
            std::memcpy(staging.buffer.mapped, reinterpret_cast<const void*>(source.vertices),
                        source.vertex_bytes);
            if (source.skin != 0) {
                std::memcpy(staging.buffer.mapped + skin_offset,
                            reinterpret_cast<const void*>(source.skin), source.skin_bytes);
            }
            if (valid_cloth) {
                std::memcpy(staging.buffer.mapped + particles_offset,
                            reinterpret_cast<const void*>(source.particles), source.particle_bytes);
                std::memcpy(staging.buffer.mapped + constraints_offset,
                            reinterpret_cast<const void*>(source.constraints),
                            source.constraint_bytes);
                std::memcpy(staging.buffer.mapped + map_offset,
                            reinterpret_cast<const void*>(source.particle_map), source.map_bytes);
            }
            // Every index must name a vertex inside the copied vertex data. The game's copy is
            // read, as the staging memory is write-combined.
            u32 max_index = 0;
            if (source.index_bytes != 0) {
                std::memcpy(staging.buffer.mapped + vertex_region,
                            reinterpret_cast<const void*>(source.indices), source.index_bytes);
                const auto* index_data = reinterpret_cast<const u16*>(source.indices);
                max_index = *std::max_element(index_data, index_data + source.index_bytes / 2);
            }
            if (g_stats.uploads_logged < LoggedUploads) {
                g_stats.uploads_logged++;
                LOG_INFO(Render_Vulkan,
                         "GR2port: mesh {:#x}: {} vertices of {} bytes, position format {} at {}, "
                         "{} indices up to {}",
                         shape, vertex_count, source.stride, source.position >> 8,
                         source.position & 0xff, source.index_bytes / 2, max_index);
            }
            if (source.stride == 0 || max_index >= vertex_count ||
                (source.skin != 0 && max_index >= source.skin_bytes / SkinStride)) {
                FreeIndices(first_index, source.index_bytes / 2);
                g_memory->Destroy(staging.buffer);
                g_memory->Destroy(vertices);
                g_stats.skipped++;
                g_stats.bad_indices++;
                continue;
            }
            const vk::BufferCopy vertex_copy{.size = vertex_region};
            cmd.copyBuffer(staging.buffer.buffer, vertices.buffer, vertex_copy, g_device->dispatch);
            if (source.index_bytes != 0) {
                const vk::BufferCopy index_copy{
                    .srcOffset = vertex_region,
                    .dstOffset = 2ull * first_index,
                    .size = source.index_bytes & ~1u,
                };
                cmd.copyBuffer(staging.buffer.buffer, g_index_buffer.buffer, index_copy,
                               g_device->dispatch);
            }
            mesh.vertices = vertices.address;
            mesh.first_index = first_index;
            mesh.indices = source.index_bytes / 2;
            mesh.stride = source.stride;
            mesh.vertex_bytes = source.vertex_bytes;
            mesh.position = source.position;
            mesh.texcoord = source.texcoord;
            mesh.texcoord1 = source.texcoord1;
            mesh.normal = source.normal;
            mesh.tangent = source.tangent;
            mesh.vertex_color = source.vertex_color;
            SetBox(source, vertex_count, mesh);
            mesh.skin = source.skin != 0 ? vertices.address + skin_offset : 0;
            mesh.particles = valid_cloth ? vertices.address + particles_offset : 0;
            mesh.constraints = valid_cloth ? vertices.address + constraints_offset : 0;
            mesh.particle_map = valid_cloth ? vertices.address + map_offset : 0;
            mesh.particle_count = valid_cloth ? source.particle_count : 0;
            if (valid_cloth && g_stats.cloth < LoggedCloth) {
                LOG_INFO(Render_Vulkan,
                         "GR2port: cloth of mesh {:#x}: {} particles, {} constraints, {} map "
                         "entries for {} vertices",
                         shape, source.particle_count, source.constraint_bytes / 4,
                         source.map_bytes / 2, vertex_count);
            }
            g_stats.no_texcoord += source.texcoord == 0;
            g_stats.no_normal += source.normal == 0;
            g_stats.skinned += source.skin != 0;
            g_stats.cloth += valid_cloth;
            g_resident[handle] = {
                .vertices = vertices,
                .first_index = first_index,
                .indices = source.index_bytes / 2,
                .skinned = source.skin != 0,
                .cloth = valid_cloth,
            };
            g_staging.push_back(staging);
            g_stats.meshes++;
            g_stats.uploaded_bytes += bytes;
            g_stats.resident_bytes += bytes;
            uploaded.push_back(handle);
            budget = bytes >= budget ? 0 : budget - bytes;
        }
        g_stats.pending = static_cast<u32>(g_pending.size());
        g_stats.missed_unloads = g_missed_unloads;
        g_stats.lost_handles = g_lost_handles;
    }
    if (uploaded.empty()) {
        return;
    }
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader |
                        vk::PipelineStageFlagBits2::eIndexInput |
                        vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eIndexRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier},
                         g_device->dispatch);
    for (const u32 handle : uploaded) {
        g_meshes[handle].ready.store(true, std::memory_order_release);
    }
}

vk::Buffer IndexBuffer() {
    return g_index_buffer.buffer;
}

Stats TakeStats() {
    Stats stats = g_stats;
    g_stats.uploaded_bytes = 0;
    g_stats.skipped = 0;
    g_stats.bad_indices = 0;
    g_stats.bad_cloth = 0;
    return stats;
}

} // namespace Gr2Port::Meshes
