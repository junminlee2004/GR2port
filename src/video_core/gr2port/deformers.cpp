// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "common/logging/log.h"
#include "video_core/gr2port/deformers.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/shaders/effector_comp.h"
#include "video_core/gr2port/shaders/skin_comp.h"
#include "video_core/gr2port/shaders/soft_body_comp.h"
#include "video_core/gr2port/shaders/soft_body_write_comp.h"

namespace Gr2Port::Deformers {

namespace {

// Deformed instance: SObj kind at +0xa, shape at +0x10, vertex buffer object at +0x18, handle at
// +0x13c in the gap after the vertex buffer object.
constexpr u64 InstanceKind = 0xa;
constexpr u16 KindDeformed = 0x19;
constexpr u64 InstanceShape = 0x10;
constexpr u64 InstanceVertexBuffer = 0x18;
constexpr u64 InstanceHandle = 0x13c;
// Shape: vertex buffer object at +0x60.
constexpr u64 ShapeVertexBuffer = 0x60;
// Vertex buffer object: stride +0x18, element count +0x1c, vertex count +0x20, the element of
// each semantic from +0x24, and the elements, 4 bytes each, from +0x34.
constexpr u64 VertexStride = 0x18;
constexpr u64 VertexElements = 0x1c;
constexpr u64 VertexCount = 0x20;
constexpr u64 VertexLayout = 0x24;
constexpr u64 VertexElementInfo = 0x34;
constexpr u32 MaxElements = 16;
// An instance unused for this many game frames loses its copy.
constexpr u64 RetireFrames = 600;
// The copies share one buffer the renderer creates at its start, before textures fill the
// device's memory; a copy starts at a multiple of CopyAlignment.
constexpr u64 ArenaBytes = 16_MB;
constexpr u64 CopyAlignment = 256;
// A cloth instance's state follows its vertex data: a header, then a position and previous
// position per particle.
constexpr u64 StateHeaderBytes = 32;
constexpr u64 ParticleStateBytes = 32;
constexpr u32 GridSlices = 10;
constexpr u64 GridBytes = u64{Scene::GridCells} * Scene::GridEntries * 4;
constexpr u32 LoggedInstances = 4;
static_assert(Capacity <= 0x80000000u);

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

u64 Aligned(u64 bytes) {
    return (bytes + CopyAlignment - 1) & ~(CopyAlignment - 1);
}

void Stamp(VAddr object, u32 value) {
    std::memcpy(reinterpret_cast<void*>(object + InstanceHandle), &value, sizeof(value));
}

// The handle field of an instance whose layout is not its shape's: at or above Capacity and
// derived from the shape, so that a later instance of another shape at the address is checked.
u32 UnsupportedStamp(VAddr shape) {
    return 0x80000000u | static_cast<u32>(shape >> 4);
}

struct Instance {
    std::atomic<VAddr> object{0};
    std::atomic<VAddr> shape{0};
    // The last game frame the capture used the instance.
    std::atomic<u64> used{0};
    // Written by the capture before it publishes object.
    const Meshes::Mesh* mesh = nullptr;
    Copy copy;
};

struct Range {
    u64 offset = 0;
    u64 size = 0;
};

// Guards the handle lists and the arena's free ranges: offset to size.
std::mutex g_mutex;
std::vector<u32> g_free;
u32 g_unused = 0;
std::vector<u32> g_pending;
std::map<u64, u64> g_free_ranges;
std::array<Instance, Capacity> g_instances;
std::array<Range, Capacity> g_ranges;
std::atomic<u32> g_unsupported{0};
std::atomic<u32> g_no_room{0};

// Render thread only.
struct Retiring {
    u32 handle;
    // The last frame that can draw the copy.
    u64 frame;
};
const Device* g_device = nullptr;
Pipeline g_skin;
Pipeline g_effector;
Pipeline g_soft_body;
Pipeline g_soft_body_write;
Buffer g_arena;
Buffer g_grid;
std::vector<u32> g_live;
std::vector<Retiring> g_retiring;
Stats g_stats{};
u32 g_logged = 0;

bool Allocate(u64 size, u64& offset) {
    for (auto it = g_free_ranges.begin(); it != g_free_ranges.end(); ++it) {
        if (it->second < size) {
            continue;
        }
        offset = it->first;
        const u64 left = it->second - size;
        g_free_ranges.erase(it);
        if (left != 0) {
            g_free_ranges.emplace(offset + size, left);
        }
        return true;
    }
    return false;
}

void Release(u64 offset, u64 size) {
    auto [it, inserted] = g_free_ranges.emplace(offset, size);
    const auto next = std::next(it);
    if (next != g_free_ranges.end() && it->first + it->second == next->first) {
        it->second += next->second;
        g_free_ranges.erase(next);
    }
    if (it != g_free_ranges.begin()) {
        const auto previous = std::prev(it);
        if (previous->first + previous->second == it->first) {
            previous->second += it->second;
            g_free_ranges.erase(it);
        }
    }
}

// A deformed instance copies its shape's vertex buffer: same stride, vertex count and elements.
bool SameLayout(VAddr vertex_buffer, VAddr shape, const Meshes::Mesh& mesh) {
    const VAddr own = shape + ShapeVertexBuffer;
    const u32 elements = Load<u16>(vertex_buffer + VertexElements);
    const u32 vertices = Load<u32>(vertex_buffer + VertexCount);
    if (Load<u16>(vertex_buffer + VertexStride) != mesh.stride ||
        elements != Load<u16>(own + VertexElements) || elements > MaxElements ||
        vertices != Load<u32>(own + VertexCount) ||
        u64{vertices} * mesh.stride > mesh.vertex_bytes) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const void*>(vertex_buffer + VertexLayout),
                       reinterpret_cast<const void*>(own + VertexLayout),
                       VertexElementInfo - VertexLayout + 4 * elements) == 0;
}

u64 VertexBytes(const Meshes::Mesh& mesh) {
    return Aligned(mesh.vertex_bytes);
}

u64 StateBytes(const Meshes::Mesh& mesh, u64 state_bytes) {
    return mesh.particle_count != 0
               ? Aligned(StateHeaderBytes + u64{mesh.particle_count} * ParticleStateBytes)
               : Aligned(state_bytes);
}

// Gives an instance its copy at once: the frame being captured can already use it, because the
// render thread fills the copy before it runs that frame's jobs and draws.
Copy Register(VAddr object, VAddr vertex_buffer, VAddr shape, const Meshes::Mesh& mesh, u64 frame,
              u64 extra_state_bytes) {
    if (Load<u16>(object + InstanceKind) != KindDeformed ||
        Load<VAddr>(object + InstanceShape) != shape || !SameLayout(vertex_buffer, shape, mesh)) {
        Stamp(object, UnsupportedStamp(shape));
        g_unsupported.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    const u64 vertex_bytes = VertexBytes(mesh);
    const u64 state_bytes = StateBytes(mesh, extra_state_bytes);
    Range range{.size = vertex_bytes + state_bytes};
    std::scoped_lock lock{g_mutex};
    if (g_free.empty() && g_unused == Capacity) {
        return {};
    }
    if (!Allocate(range.size, range.offset)) {
        g_no_room.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    u32 handle = Capacity;
    if (!g_free.empty()) {
        handle = g_free.back();
        g_free.pop_back();
    } else {
        handle = g_unused++;
    }
    g_ranges[handle] = range;
    const u64 address = g_arena.address + range.offset;
    Instance& instance = g_instances[handle];
    instance.mesh = &mesh;
    instance.copy = {
        .vertices = address,
        .state = state_bytes != 0 ? address + vertex_bytes : 0,
    };
    instance.shape.store(shape, std::memory_order_relaxed);
    instance.used.store(frame, std::memory_order_relaxed);
    instance.object.store(object, std::memory_order_release);
    Stamp(object, handle);
    g_pending.push_back(handle);
    return instance.copy;
}

bool Unused(const Instance& instance, u64 game_frame) {
    return instance.used.load(std::memory_order_relaxed) + RetireFrames < game_frame;
}

void Retire(u64 frame, u64 completed, u64 game_frame) {
    // The packet of this frame, or the one the capture is writing, which the next frame takes at
    // the latest, can still draw a copy.
    std::erase_if(g_live, [frame, game_frame](u32 handle) {
        Instance& instance = g_instances[handle];
        if (!Unused(instance, game_frame)) {
            return false;
        }
        instance.object.store(0, std::memory_order_release);
        g_retiring.push_back({handle, frame + 1});
        g_stats.retired++;
        return true;
    });
    std::erase_if(g_retiring, [completed](const Retiring& r) {
        if (completed < r.frame) {
            return false;
        }
        std::scoped_lock lock{g_mutex};
        Range& range = g_ranges[r.handle];
        g_stats.resident_bytes -= range.size;
        g_stats.instances--;
        Release(range.offset, range.size);
        range = {};
        g_free.push_back(r.handle);
        return true;
    });
}

} // namespace

Copy Get(VAddr vertex_buffer, VAddr shape, const Meshes::Mesh& mesh, u64 frame, u64 state_bytes) {
    const VAddr object = vertex_buffer - InstanceVertexBuffer;
    const u32 handle = Load<u32>(object + InstanceHandle);
    if (handle < Capacity) {
        Instance& instance = g_instances[handle];
        if (instance.object.load(std::memory_order_acquire) == object &&
            instance.shape.load(std::memory_order_relaxed) == shape) {
            instance.used.store(frame, std::memory_order_relaxed);
            return instance.copy;
        }
    } else if (handle == UnsupportedStamp(shape)) {
        return {};
    }
    return Register(object, vertex_buffer, shape, mesh, frame, state_bytes);
}

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_skin = CreateComputePipeline(device, SKIN_COMP, sizeof(u64));
    g_effector = CreateComputePipeline(device, EFFECTOR_COMP, 3 * sizeof(u64));
    g_soft_body = CreateComputePipeline(device, SOFT_BODY_COMP, 3 * sizeof(u64));
    g_soft_body_write = CreateComputePipeline(device, SOFT_BODY_WRITE_COMP, 3 * sizeof(u64));
    g_arena = memory.CreateBuffer(
        ArenaBytes, vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        Placement::Device);
    g_grid =
        memory.CreateBuffer(GridBytes, vk::BufferUsageFlagBits::eStorageBuffer, Placement::Device);
    g_free_ranges.emplace(0, ArenaBytes);
    return g_skin.pipeline && g_effector.pipeline && g_soft_body.pipeline &&
           g_soft_body_write.pipeline && g_arena.buffer && g_grid.buffer;
}

void Update(vk::CommandBuffer cmd, u64 frame, u64 completed, u64 game_frame) {
    const auto& dl = g_device->dispatch;
    Retire(frame, completed, game_frame);
    std::vector<u32> created;
    {
        std::scoped_lock lock{g_mutex};
        created.swap(g_pending);
    }
    if (created.empty()) {
        return;
    }
    // The mesh's own upload can be in this command buffer.
    const vk::MemoryBarrier2 uploaded{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &uploaded}, dl);
    for (const u32 handle : created) {
        Instance& instance = g_instances[handle];
        const Meshes::Mesh& mesh = *instance.mesh;
        const Range range = g_ranges[handle];
        g_stats.instances++;
        g_stats.resident_bytes += range.size;
        // An instance whose mesh was unloaded since the capture has nothing to copy, and the
        // game no longer draws it.
        if (!mesh.ready.load(std::memory_order_acquire) ||
            mesh.shape.load(std::memory_order_relaxed) !=
                instance.shape.load(std::memory_order_relaxed)) {
            instance.object.store(0, std::memory_order_release);
            g_retiring.push_back({handle, frame + 1});
            continue;
        }
        Meshes::CopyVertices(cmd, mesh, g_arena.buffer, range.offset);
        const u64 state_bytes = range.size - VertexBytes(mesh);
        if (state_bytes != 0) {
            // No particle holds a position yet: the simulate pass resets them.
            cmd.fillBuffer(g_arena.buffer, range.offset + VertexBytes(mesh),
                           mesh.particle_count != 0 ? StateHeaderBytes : state_bytes, 0, dl);
        }
        if (g_logged < LoggedInstances) {
            g_logged++;
            LOG_INFO(Render_Vulkan,
                     "GR2port: deformed instance {:#x} of shape {:#x}: {} bytes of {} a vertex",
                     instance.object.load(std::memory_order_relaxed),
                     instance.shape.load(std::memory_order_relaxed), mesh.vertex_bytes,
                     mesh.stride);
        }
        g_live.push_back(handle);
    }
    const vk::MemoryBarrier2 copied{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy | vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &copied}, dl);
}

void Record(vk::CommandBuffer cmd, const Scene::Header& header, u64 address) {
    const bool skins = header.skin_jobs != 0 && header.skin_jobs <= Scene::MaxSkinJobs;
    const bool cloth =
        header.soft_body_jobs != 0 && header.soft_body_jobs <= Scene::MaxSoftBodyJobs;
    const bool grid = cloth || header.grass != 0 || header.branch_jobs != 0;
    if (!skins && !grid) {
        return;
    }
    const auto& dl = g_device->dispatch;
    // The vertex shaders of earlier frames read the copies the jobs write; the passes of earlier
    // frames read the grid and wrote the states.
    const vk::MemoryBarrier2 drawn{
        .srcStageMask =
            vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    const vk::MemoryBarrier2 computed{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &drawn}, dl);
    const std::array<u64, 3> cloth_push{address + Scene::SoftBodyJobsOffset,
                                        address + Scene::EffectorsOffset, g_grid.address};
    const auto bind = [&](const Pipeline& pipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline.pipeline, dl);
        cmd.pushConstants(pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(cloth_push),
                          cloth_push.data(), dl);
    };
    if (grid) {
        bind(g_effector);
        cmd.dispatch(1, 1, GridSlices, dl);
    }
    if (skins) {
        const u64 jobs = address + Scene::SkinJobsOffset;
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, g_skin.pipeline, dl);
        cmd.pushConstants(g_skin.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(jobs), &jobs,
                          dl);
        cmd.dispatch(header.skin_groups, header.skin_jobs, 1, dl);
    }
    if (cloth) {
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &computed}, dl);
        bind(g_soft_body);
        cmd.dispatch(header.soft_body_groups, header.soft_body_jobs, 1, dl);
        cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &computed}, dl);
        bind(g_soft_body_write);
        cmd.dispatch(header.write_groups, header.soft_body_jobs, 1, dl);
    }
    const vk::MemoryBarrier2 skinned{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eVertexShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderStorageRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &skinned}, dl);
}

u64 GridAddress() {
    return g_grid.address;
}

Stats TakeStats() {
    Stats stats = g_stats;
    stats.unsupported = g_unsupported.exchange(0, std::memory_order_relaxed);
    stats.pending = g_no_room.exchange(0, std::memory_order_relaxed);
    g_stats.retired = 0;
    return stats;
}

} // namespace Gr2Port::Deformers
