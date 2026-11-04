// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include <tsl/robin_map.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/gr2_pt/caster_batch.h"
#include "video_core/gr2_pt/caster_renderer.h"
#include "video_core/gr2_pt/cpu_writes.h"
#include "video_core/gr2_pt/gr2_pt.h"
#include "video_core/gr2_pt/shaders/caster_skinned_vert.h"
#include "video_core/gr2_pt/shaders/caster_static_vert.h"
#include "video_core/gr2_pt/shadow_pass.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/skipcache/skipcache.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Gr2Pt::CasterRenderer {

namespace {

using CasterBatch::Record;

constexpr u64 RingBytes = 32_MB;
constexpr u32 ReportFrames = 600;
constexpr u32 MatrixBytes = 64;
constexpr u32 BoneBytes = 48;
constexpr u64 MatrixAlignment = 16;
// Larger ranges are resolved again by each batch.
constexpr u64 MaxKeptBytes = 64_MB;

// Push constant flags, as the vertex programs read them.
constexpr u32 FlagDepthMinusOneToOne = 1;

// Group flags past the record flags.
constexpr u8 GroupSkinned = 4;

constexpr vk::PipelineStageFlags2 DepthStages = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                                vk::PipelineStageFlagBits2::eLateFragmentTests;
constexpr vk::AccessFlags2 DepthAccess = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite;

/// A caster's draw as the vertex programs read it.
struct Draw {
    u64 positions;
    u64 weights;
    u64 bone_indices;
    u64 world;
    u64 view_projection;
    u64 palette;
    u32 position_stride;
    u32 weight_stride;
    u32 bone_stride;
    u32 bones;
};
static_assert(sizeof(Draw) == 64);

struct Constants {
    u64 draws;
    u64 slots;
    u32 flags;
    u32 padding;
};

struct PipelineKey {
    vk::Format depth_format;
    vk::Format stencil_format;
    bool depth_clamp;
    bool depth_clip;
    bool minus_one_to_one;

    bool operator==(const PipelineKey&) const = default;
};

struct Pipelines {
    PipelineKey key;
    vk::UniquePipeline static_casters;
    vk::UniquePipeline skinned_casters;
};

/// The shadow map the registers name, and how a draw would begin rendering to it.
struct Target {
    VideoCore::Image* image;
    vk::ImageView view;
    VideoCore::SubresourceRange range;
    vk::ImageLayout layout;
    Vulkan::RenderState state;
    PipelineKey key;
    u32 flags;
    bool has_stencil;
};

struct Obtained {
    const VideoCore::Buffer* buffer;
    u64 offset;
    u32 size;
    // The last batch that read the range.
    u64 batch;
};

struct Read {
    const VideoCore::Buffer* buffer;
    u64 begin;
    u64 end;
};

struct Resolved {
    vk::DrawIndexedIndirectCommand command;
    u32 group;
};

// The last range a resolve step obtained: consecutive casters are often draws of one mesh and
// share its streams and indices.
struct Recent {
    VAddr address = ~VAddr{0};
    u32 size = 0;
    const VideoCore::Buffer* buffer = nullptr;
    u64 offset = 0;
};

struct Group {
    const VideoCore::Buffer* indices;
    u8 flags;
    u32 count;
    u32 first;
};

struct Stats {
    u64 frames;
    u64 batches;
    u64 records;
    u64 groups;
    u64 fallbacks;
    u64 obtains;
    u64 flushes;
    // Rendering began again with the scope closed, with the game's clear as the only difference
    // from the open scope, or with another difference.
    u64 restarts_closed;
    u64 restarts_cleared;
    u64 restarts_other;
    u64 clears;
    u64 ring_bytes;
    u64 ring_waits;
    u64 gpu_matrices;
    u64 misaligned;
    u64 dropped;
    u64 reads;
    // Command processor time of the batches by step.
    u64 resolve_ns;
    u64 target_ns;
    u64 barrier_ns;
    u64 begin_ns;
    u64 record_ns;
    u64 finish_ns;
};

enum class DepthKind {
    Float32,
    Unorm16,
    Unorm24,
};

// One frame in the compare interval: the shadow map after the pass's first caster, our draws of
// the other casters into it, and both read back.
struct Compare {
    VideoCore::UniqueImage image{};
    vk::UniqueImageView view;
    vk::Format format;
    u32 width;
    u32 height;
    bool has_stencil;
    // The copy holds the pass's first caster and waits for its batch.
    bool snapshot;
    u32 pass;
    u32 frame;
    std::unique_ptr<VideoCore::Buffer> readback;
    u64 plane_bytes;
    DepthKind kind;
    // The readback is recorded and completes at tick.
    bool pending;
    u64 tick;
    u32 fallbacks;
    u32 records;
    std::atomic<bool> busy;
    std::jthread worker;
};

// Command processor thread only.
struct State {
    bool prepared;
    bool ready;
    vk::Device device;
    vk::PipelineLayout layout;
    vk::ShaderModule static_module;
    vk::ShaderModule skinned_module;
    std::vector<Pipelines> pipelines;
    std::unique_ptr<VideoCore::Buffer> ring;
    u64 ring_address;
    // Positions count every byte the ring has handed out; the ring offset is the position modulo
    // its size.
    u64 head;
    u64 batch_position;
    // The position of each batch's data and the tick that reads it.
    std::deque<std::pair<u64, u64>> fences;
    // The guest ranges resolved to the buffer cache's copies, kept until the CPU writes them, and
    // the ranges in each watched block.
    tsl::robin_map<VAddr, Obtained> ranges;
    tsl::robin_map<u64, std::vector<VAddr>> blocks;
    std::vector<CpuWrites::Range> writes;
    std::vector<VAddr> dropped;
    u64 batch;
    tsl::robin_map<VAddr, u64> matrices;
    std::vector<Read> reads;
    std::vector<Resolved> resolved;
    std::vector<Group> groups;
    Stats stats;
    Compare compare;
};
// Never destroyed: the device is gone by the time static objects are.
State& g_state = *new State{};

void Fail(const char* reason) {
    g_state.ready = false;
    CasterBatch::Disable();
    LOG_ERROR(Render_Vulkan,
              "GR2 PT: the caster renderer stops, the game draws its casters again: {}", reason);
}

const char* FindTarget(Vulkan::Rasterizer& rasterizer, Target& target) {
    const AmdGpu::Liverpool& gpu = RasterizerAccess::Gpu(rasterizer);
    const Vulkan::Instance& instance = RasterizerAccess::Instance(rasterizer);
    const auto& regs = gpu.regs;
    if (regs.IsClipDisabled()) {
        return "clipping is disabled";
    }
    if (regs.clipper_control.user_clip_plane_enable != 0) {
        return "user clip planes are enabled";
    }
    if (regs.primitive_type != AmdGpu::PrimitiveType::TriangleList ||
        regs.polygon_control.PolyMode() != AmdGpu::PolygonMode::Fill) {
        return "the pass does not draw filled triangle lists";
    }
    if (!regs.depth_control.depth_enable || !regs.depth_buffer.DepthValid()) {
        return "the pass has no depth target";
    }
    if (regs.depth_buffer.NumSamples() != 1) {
        return "the depth target is multisampled";
    }
    VideoCore::TextureCache& textures = RasterizerAccess::Textures(rasterizer);
    const VAddr htile = regs.depth_htile_data_base.GetAddress();
    VideoCore::TextureCache::ImageDesc desc(regs.depth_buffer, regs.depth_view, regs.depth_control,
                                            htile, gpu.last_db_extent);
    const VideoCore::ImageId image_id = textures.FindImage(desc);
    const VideoCore::ImageView& view = textures.FindDepthTarget(image_id, desc);
    VideoCore::Image& image = textures.GetImage(image_id);
    if (desc.view_info.range.extent.levels != 1 || image.binding.needs_rebind ||
        image.info.num_samples != 1 || image.backing_num_samples != 1) {
        return "the depth target is not a single-sampled level";
    }
    target.image = &image;
    target.view = *view.image_view;
    target.range = desc.view_info.range;
    target.has_stencil = image.info.props.has_stencil;
    const bool stencil_write =
        target.has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
    target.layout = desc.view_info.is_storage
                        ? target.has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                             : vk::ImageLayout::eDepthAttachmentOptimal
                    : stencil_write      ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                    : target.has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                         : vk::ImageLayout::eDepthReadOnlyOptimal;

    const bool stencil_valid = regs.depth_buffer.StencilValid();
    const vk::Format format = instance.GetSupportedFormat(
        Vulkan::LiverpoolToVK::DepthFormat(regs.depth_buffer.z_info.format,
                                           stencil_valid
                                               ? regs.depth_buffer.stencil_info.format
                                               : AmdGpu::DepthBuffer::StencilFormat::Invalid),
        vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    target.key = {
        .depth_format = format,
        .stencil_format = stencil_valid ? format : vk::Format::eUndefined,
        .depth_clamp = !regs.depth_render_override.disable_viewport_clamp,
        .depth_clip = regs.clipper_control.ZclipEnable(),
        .minus_one_to_one = regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };
    target.flags = !instance.IsDepthClipControlSupported() && target.key.minus_one_to_one
                       ? FlagDepthMinusOneToOne
                       : 0;

    Vulkan::RenderState& state = target.state;
    state = {};
    state.width = std::min<u32>(instance.GetMaxFramebufferWidth(), image.info.size.width);
    state.height = std::min<u32>(instance.GetMaxFramebufferHeight(), image.info.size.height);
    state.num_layers =
        std::min<u32>(std::numeric_limits<u16>::max(), view.info.range.extent.layers);
    auto& attachment = state.depth_stencil_attachment;
    attachment.image_view = target.view;
    const u32 slice = view.info.range.base.layer;
    const bool depth_clear =
        (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
         regs.depth_control.depth_write_enable) ||
        textures.IsMetaCleared(htile, slice);
    const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
    textures.TouchMeta(htile, slice, false);
    attachment.clear_value[0] = depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
    attachment.has_depth = true;
    attachment.depth_clear = depth_clear;
    if (stencil_valid) {
        attachment.clear_value[1] = stencil_clear ? regs.stencil_clear : 0u;
        attachment.has_stencil = true;
        attachment.stencil_clear = stencil_clear;
    }
    image.usage.depth_target = true;
    return nullptr;
}

vk::UniquePipeline CreatePipeline(const Vulkan::Instance& instance, const PipelineKey& key,
                                  vk::ShaderModule module) {
    const vk::PipelineShaderStageCreateInfo stage{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = module,
        .pName = "main",
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control{
        .negativeOneToOne = key.minus_one_to_one,
    };
    const vk::PipelineViewportStateCreateInfo viewport{
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };
    const vk::PipelineRasterizationDepthClipStateCreateInfoEXT depth_clip{
        .depthClipEnable = key.depth_clip,
    };
    const vk::PipelineRasterizationStateCreateInfo rasterization{
        .pNext = instance.IsDepthClipEnableSupported() ? &depth_clip : nullptr,
        .depthClampEnable =
            key.depth_clamp && (!key.depth_clip || instance.IsDepthClipEnableSupported()),
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    const vk::PipelineColorBlendStateCreateInfo color_blend{};
    // The dynamic state of the game's pipelines, so that the draws around ours keep theirs.
    std::vector<vk::DynamicState> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };
    if (instance.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskEnabled()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::PipelineRenderingCreateInfo rendering{
        .depthAttachmentFormat = key.depth_format,
        .stencilAttachmentFormat = key.stencil_format,
    };
    auto [result, pipeline] =
        g_state.device.createGraphicsPipelineUnique({}, {
                                                            .pNext = &rendering,
                                                            .stageCount = 1,
                                                            .pStages = &stage,
                                                            .pVertexInputState = &vertex_input,
                                                            .pInputAssemblyState = &input_assembly,
                                                            .pViewportState = &viewport,
                                                            .pRasterizationState = &rasterization,
                                                            .pMultisampleState = &multisample,
                                                            .pDepthStencilState = &depth_stencil,
                                                            .pColorBlendState = &color_blend,
                                                            .pDynamicState = &dynamic,
                                                            .layout = g_state.layout,
                                                        });
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no caster pipeline: {}", vk::to_string(result));
        return {};
    }
    return std::move(pipeline);
}

const Pipelines* FindPipelines(const Vulkan::Instance& instance, const PipelineKey& key) {
    for (const Pipelines& p : g_state.pipelines) {
        if (p.key == key) {
            return &p;
        }
    }
    Pipelines p{
        .key = key,
        .static_casters = CreatePipeline(instance, key, g_state.static_module),
        .skinned_casters = CreatePipeline(instance, key, g_state.skinned_module),
    };
    if (!p.static_casters || !p.skinned_casters) {
        return nullptr;
    }
    LOG_INFO(Render_Vulkan,
             "GR2 PT: caster pipelines for depth {} stencil {}, clamp {}, clip {}, [-w, w] {}",
             vk::to_string(key.depth_format), vk::to_string(key.stencil_format), key.depth_clamp,
             key.depth_clip, key.minus_one_to_one);
    return &g_state.pipelines.emplace_back(std::move(p));
}

/// Reserves bytes of the ring, waiting for the GPU to finish the batches whose data they cover.
/// Returns their position.
u64 Allocate(Vulkan::Scheduler& scheduler, u64 bytes) {
    u64 begin = Common::AlignUp(g_state.head, 64);
    if (begin % RingBytes + bytes > RingBytes) {
        begin = Common::AlignUp(begin, RingBytes);
    }
    const u64 end = begin + bytes;
    while (!g_state.fences.empty() && g_state.fences.front().first + RingBytes < end) {
        const u64 tick = g_state.fences.front().second;
        if (!scheduler.IsFree(tick)) {
            scheduler.Wait(tick);
            ++g_state.stats.ring_waits;
        }
        g_state.fences.pop_front();
    }
    g_state.head = end;
    g_state.stats.ring_bytes += bytes;
    return begin;
}

u64 LastBlock(VAddr address, u32 size) {
    return (address + std::max<u32>(size, 1) - 1) >> CpuWrites::BlockBits;
}

void Index(VAddr address, u32 size) {
    for (u64 block = address >> CpuWrites::BlockBits; block <= LastBlock(address, size); ++block) {
        g_state.blocks[block].push_back(address);
    }
}

void Unindex(VAddr address, u32 size) {
    for (u64 block = address >> CpuWrites::BlockBits; block <= LastBlock(address, size); ++block) {
        const auto it = g_state.blocks.find(block);
        if (it == g_state.blocks.end()) {
            continue;
        }
        std::vector<VAddr>& kept = it.value();
        if (const auto found = std::ranges::find(kept, address); found != kept.end()) {
            *found = kept.back();
            kept.pop_back();
        }
        if (kept.empty()) {
            g_state.blocks.erase(it);
        }
    }
}

void Drop(VAddr address) {
    const auto it = g_state.ranges.find(address);
    if (it == g_state.ranges.end()) {
        return;
    }
    const u32 size = it->second.size;
    g_state.ranges.erase(it);
    Unindex(address, size);
    ++g_state.stats.dropped;
}

/// Drops the kept ranges on the pages the CPU wrote. A reported write unprotects its whole page,
/// so later writes to the page go unreported until the page is uploaded again.
void Forget() {
    CpuWrites::Take(g_state.writes);
    for (const CpuWrites::Range& write : g_state.writes) {
        const VAddr begin = Common::AlignDown(write.begin, VideoCore::BYTES_PER_PAGE);
        const VAddr end = Common::AlignUp(write.end, VideoCore::BYTES_PER_PAGE);
        const auto overlaps = [begin, end](VAddr address, const Obtained& o) {
            return address < end && begin < address + std::max<u32>(o.size, 1);
        };
        const u64 first = begin >> CpuWrites::BlockBits;
        const u64 last = (end - 1) >> CpuWrites::BlockBits;
        g_state.dropped.clear();
        if (last - first >= g_state.blocks.size()) {
            for (const auto& [address, o] : g_state.ranges) {
                if (overlaps(address, o)) {
                    g_state.dropped.push_back(address);
                }
            }
        } else {
            for (u64 block = first; block <= last; ++block) {
                const auto it = g_state.blocks.find(block);
                if (it == g_state.blocks.end()) {
                    continue;
                }
                for (const VAddr address : it->second) {
                    const auto kept = g_state.ranges.find(address);
                    if (kept != g_state.ranges.end() && overlaps(address, kept->second)) {
                        g_state.dropped.push_back(address);
                    }
                }
            }
        }
        for (const VAddr address : g_state.dropped) {
            Drop(address);
        }
    }
}

/// The buffer cache's copy of [address, address + size), resolved once and kept until the CPU
/// writes its pages. Each batch reads a range once.
std::pair<const VideoCore::Buffer*, u64> Obtain(VideoCore::BufferCache& buffers, VAddr address,
                                                u32 size) {
    if (size > MaxKeptBytes) {
        const auto [buffer, offset] =
            buffers.ObtainBuffer(address, size, VideoCore::ObtainBufferFlags::IgnoreStreamBuffer);
        g_state.reads.push_back({buffer, offset, offset + size});
        ++g_state.stats.obtains;
        return {buffer, offset};
    }
    auto [it, inserted] = g_state.ranges.try_emplace(address);
    Obtained& o = it.value();
    if (inserted || o.size < size) {
        if (!inserted) {
            Unindex(address, o.size);
        }
        CpuWrites::Watch(address, std::max<u32>(size, 1));
        const auto [buffer, offset] =
            buffers.ObtainBuffer(address, size, VideoCore::ObtainBufferFlags::IgnoreStreamBuffer);
        o = {buffer, offset, size, 0};
        Index(address, size);
        ++g_state.stats.obtains;
    }
    if (o.batch != g_state.batch) {
        o.batch = g_state.batch;
        g_state.reads.push_back({o.buffer, o.offset, o.offset + o.size});
    }
    return {o.buffer, o.offset};
}

std::pair<const VideoCore::Buffer*, u64> Obtain(VideoCore::BufferCache& buffers, Recent& recent,
                                                VAddr address, u32 size) {
    if (address != recent.address || size > recent.size) {
        const auto [buffer, offset] = Obtain(buffers, address, size);
        recent = {address, size, buffer, offset};
    }
    return {recent.buffer, recent.offset};
}

u64 Stream(VideoCore::BufferCache& buffers, Recent& recent, const AmdGpu::Buffer& stream) {
    const auto [buffer, offset] = Obtain(buffers, recent, stream.base_address, stream.GetSize());
    return buffer->BufferDeviceAddress() + offset;
}

// Matrices the GPU wrote are read where the buffer cache keeps them; the rest are copied into the
// batch's data, as a constant buffer of the game's draw is.
u64 Matrix(VideoCore::BufferCache& buffers, VAddr address, u32 bytes, u8* data, u64& cursor,
           u64 data_address) {
    if (const auto it = g_state.matrices.find(address); it != g_state.matrices.end()) {
        return it->second;
    }
    u64 device = 0;
    if (buffers.IsRegionGpuModified(address, bytes)) {
        const auto [buffer, offset] = Obtain(buffers, address, bytes);
        device = buffer->BufferDeviceAddress() + offset;
        if (device % MatrixAlignment == 0) {
            ++g_state.stats.gpu_matrices;
        } else {
            ++g_state.stats.misaligned;
            device = 0;
        }
    }
    if (device == 0) {
        cursor = Common::AlignUp(cursor, MatrixAlignment);
        Core::Memory::Instance()->CopySparseMemory(address, data + cursor, bytes);
        device = data_address + cursor;
        cursor += bytes;
    }
    g_state.matrices.emplace(address, device);
    return device;
}

/// Turns the batch's records into draws in the ring, in record order, and indirect commands with
/// the slots of their draws, grouped by pipeline, cull mode and index buffer. Sets base to the
/// ring offset of the draws.
bool Resolve(Vulkan::Rasterizer& rasterizer, u32 first, u32 count, u64& base) {
    VideoCore::BufferCache& buffers = RasterizerAccess::Buffers(rasterizer);
    const Record* const records = CasterBatch::Ring();
    ++g_state.batch;
    Forget();
    g_state.matrices.clear();
    g_state.reads.clear();
    g_state.groups.clear();
    g_state.resolved.resize(count);

    u64 palette_bytes = 0;
    for (u32 i = 0; i < count; i++) {
        const u16 bones = records[(first + i) % CasterBatch::RingRecords].bones;
        palette_bytes += u64{bones} * BoneBytes + MatrixAlignment;
    }
    const u64 slots_offset = u64{count} * sizeof(Draw);
    const u64 commands_offset = slots_offset + u64{count} * sizeof(u32);
    const u64 matrices_offset = Common::AlignUp(
        commands_offset + u64{count} * sizeof(vk::DrawIndexedIndirectCommand), MatrixAlignment);
    const u64 bytes =
        matrices_offset + u64{count} * 2 * (MatrixBytes + MatrixAlignment) + palette_bytes;
    if (bytes > RingBytes / 4) {
        return false;
    }
    g_state.batch_position = Allocate(rasterizer.GetScheduler(), bytes);
    base = g_state.batch_position % RingBytes;
    u8* const data = g_state.ring->mapped_data.data() + base;
    const u64 data_address = g_state.ring_address + base;
    u64 cursor = matrices_offset;
    auto* const draws = reinterpret_cast<Draw*>(data);
    // Consecutive casters often share their matrices: a model's meshes share its world matrix
    // and a pass's casters its view-projection matrix.
    VAddr world = 0;
    VAddr view_projection = 0;
    u64 world_device = 0;
    u64 view_projection_device = 0;
    Recent positions;
    Recent weights;
    Recent bone_indices;
    Recent index_ranges;

    for (u32 i = 0; i < count; i++) {
        const Record& r = records[(first + i) % CasterBatch::RingRecords];
        Resolved& out = g_state.resolved[i];
        Draw d{};
        d.positions = Stream(buffers, positions, r.positions);
        d.position_stride = static_cast<u32>(r.positions.stride);
        if (r.world != world || world_device == 0) {
            world = r.world;
            world_device = Matrix(buffers, r.world, MatrixBytes, data, cursor, data_address);
        }
        d.world = world_device;
        if (r.view_projection != view_projection || view_projection_device == 0) {
            view_projection = r.view_projection;
            view_projection_device =
                Matrix(buffers, r.view_projection, MatrixBytes, data, cursor, data_address);
        }
        d.view_projection = view_projection_device;
        if (r.bones != 0) {
            d.weights = Stream(buffers, weights, r.weights);
            d.weight_stride = static_cast<u32>(r.weights.stride);
            d.bone_indices = Stream(buffers, bone_indices, r.bone_indices);
            d.bone_stride = static_cast<u32>(r.bone_indices.stride);
            d.palette =
                Matrix(buffers, r.palette, u32{r.bones} * BoneBytes, data, cursor, data_address);
            d.bones = r.bones;
        }
        draws[i] = d;
        const auto [indices, index_offset] =
            Obtain(buffers, index_ranges, r.indices, r.index_bytes);
        const u32 index_size = (r.flags & CasterBatch::Index32) ? 4 : 2;
        out.command = {
            .indexCount = r.count,
            .instanceCount = 1,
            .firstIndex = static_cast<u32>(index_offset / index_size) + r.first,
            .vertexOffset = 0,
            .firstInstance = 0,
        };
        const u8 flags = static_cast<u8>(r.flags | (r.bones != 0 ? GroupSkinned : 0));
        const auto group = std::ranges::find_if(g_state.groups, [&](const Group& g) {
            return g.indices == indices && g.flags == flags;
        });
        if (group == g_state.groups.end()) {
            out.group = static_cast<u32>(g_state.groups.size());
            g_state.groups.push_back({indices, flags, 1, 0});
        } else {
            out.group = static_cast<u32>(group - g_state.groups.begin());
            ++group->count;
        }
    }

    u32 slot = 0;
    for (Group& g : g_state.groups) {
        g.first = slot;
        slot += g.count;
        g.count = 0;
    }
    auto* const slots = reinterpret_cast<u32*>(data + slots_offset);
    auto* const commands =
        reinterpret_cast<vk::DrawIndexedIndirectCommand*>(data + commands_offset);
    for (u32 i = 0; i < count; i++) {
        const Resolved& r = g_state.resolved[i];
        Group& g = g_state.groups[r.group];
        const u32 s = g.first + g.count++;
        slots[s] = i;
        commands[s] = r.command;
    }
    if (!g_state.ring->is_coherent) {
        g_state.ring->Flush(base, cursor);
    }
    return true;
}

/// Sorts the batch's reads and merges the ones that overlap or touch.
void MergeReads() {
    auto& reads = g_state.reads;
    std::ranges::sort(reads, [](const Read& a, const Read& b) {
        return a.buffer != b.buffer ? a.buffer < b.buffer : a.begin < b.begin;
    });
    size_t merged = 0;
    for (size_t i = 1; i < reads.size(); i++) {
        Read& last = reads[merged];
        if (reads[i].buffer == last.buffer && reads[i].begin <= last.end) {
            last.end = std::max(last.end, reads[i].end);
        } else {
            reads[++merged] = reads[i];
        }
    }
    reads.resize(reads.empty() ? 0 : merged + 1);
}

bool NeedsBarrier(Vulkan::Runtime& runtime) {
    for (const Read& r : g_state.reads) {
        if (runtime.IsBufferAccessed(r.buffer, r.begin, r.end - r.begin)) {
            return true;
        }
    }
    return false;
}

void AccessReads(Vulkan::Runtime& runtime) {
    for (const Read& r : g_state.reads) {
        runtime.AccessBuffer(r.buffer, r.begin, r.end - r.begin,
                             vk::PipelineStageFlagBits2::eVertexInput |
                                 vk::PipelineStageFlagBits2::eVertexShader,
                             vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eShaderRead);
    }
}

void CountRestart(const Vulkan::Scheduler& scheduler, const Vulkan::RenderState& state) {
    Stats& stats = g_state.stats;
    if (!scheduler.IsRendering()) {
        ++stats.restarts_closed;
        return;
    }
    Vulkan::RenderState open = scheduler.GetRenderState();
    if (open == state) {
        return;
    }
    auto& attachment = open.depth_stencil_attachment;
    attachment.clear_value = {};
    attachment.depth_clear = false;
    attachment.stencil_clear = false;
    ++(open == state ? stats.restarts_cleared : stats.restarts_other);
}

u64 Nanoseconds(std::chrono::steady_clock::time_point& since) {
    const auto now = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - since).count();
    since = now;
    return static_cast<u64>(ns);
}

void RecordDraws(Vulkan::Scheduler& scheduler, const Pipelines& pipelines, u64 base, u32 count,
                 u32 flags) {
    const auto cmdbuf = scheduler.CommandBuffer();
    const vk::Buffer ring = g_state.ring->Handle();
    const u64 slots = base + u64{count} * sizeof(Draw);
    const u64 commands = slots + u64{count} * sizeof(u32);
    vk::Pipeline bound{};
    for (const Group& g : g_state.groups) {
        const vk::Pipeline pipeline =
            (g.flags & GroupSkinned) ? *pipelines.skinned_casters : *pipelines.static_casters;
        if (pipeline != bound) {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
            bound = pipeline;
        }
        cmdbuf.setCullMode((g.flags & CasterBatch::CullBack) ? vk::CullModeFlagBits::eBack
                                                             : vk::CullModeFlagBits::eNone);
        cmdbuf.bindIndexBuffer(g.indices->Handle(), 0,
                               (g.flags & CasterBatch::Index32) ? vk::IndexType::eUint32
                                                                : vk::IndexType::eUint16);
        const Constants constants{
            .draws = g_state.ring_address + base,
            .slots = g_state.ring_address + slots + u64{g.first} * sizeof(u32),
            .flags = flags,
        };
        cmdbuf.pushConstants(g_state.layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(constants),
                             &constants);
        cmdbuf.drawIndexedIndirect(ring,
                                   commands + u64{g.first} * sizeof(vk::DrawIndexedIndirectCommand),
                                   g.count, sizeof(vk::DrawIndexedIndirectCommand));
    }
    g_state.stats.groups += g_state.groups.size();
}

/// Hands the command buffer back to the rasterizer: it binds its pipeline, vertex input and
/// constants again and sets every dynamic state.
void Restore(Vulkan::Scheduler& scheduler) {
    auto& skipcache = VideoCore::Skipcache::Framework::Instance();
    skipcache.BumpForeignPipelineGen(0);
    skipcache.BumpForeignPushGen(0);
    scheduler.GetDynamicState().Invalidate();
}

DepthKind KindOf(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD16UnormS8Uint:
        return DepthKind::Unorm16;
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD24UnormS8Uint:
        return DepthKind::Unorm24;
    default:
        return DepthKind::Float32;
    }
}

double Depth(const u8* plane, u64 texel, DepthKind kind) {
    switch (kind) {
    case DepthKind::Unorm16: {
        u16 value;
        std::memcpy(&value, plane + texel * 2, sizeof(value));
        return value / 65535.0;
    }
    case DepthKind::Unorm24: {
        u32 value;
        std::memcpy(&value, plane + texel * 4, sizeof(value));
        return (value & 0xffffff) / 16777215.0;
    }
    case DepthKind::Float32:
    default: {
        float value;
        std::memcpy(&value, plane + texel * 4, sizeof(value));
        return value;
    }
    }
}

// The worker thread: the game's shadow map against ours.
void Diff(const u8* game, const u8* ours, u64 texels, DepthKind kind, u32 pass, u32 frame,
          u32 records, u32 fallbacks) {
    u64 equal = 0;
    u64 near6 = 0;
    u64 near4 = 0;
    u64 far = 0;
    u64 nearer = 0;
    double max_diff = 0.0;
    for (u64 t = 0; t < texels; t++) {
        const double a = Depth(game, t, kind);
        const double b = Depth(ours, t, kind);
        if (a == b) {
            ++equal;
            continue;
        }
        const double diff = std::abs(a - b);
        max_diff = std::max(max_diff, diff);
        nearer += b < a;
        if (diff <= 1e-6) {
            ++near6;
        } else if (diff <= 1e-4) {
            ++near4;
        } else {
            ++far;
        }
    }
    LOG_INFO(Render_Vulkan,
             "GR2 PT casters: compare pass {} frame {}: {} texels, {} equal, {} within 1e-6, {} "
             "within 1e-4, {} past 1e-4 (max {:.3g}), {} of the differing ones nearer in ours; "
             "{} casters drawn, {} drawn by the game's callback only",
             pass, frame, texels, equal, near6, near4, far, max_diff, nearer, records, fallbacks);
}

void Poll(Vulkan::Scheduler& scheduler) {
    Compare& c = g_state.compare;
    if (!c.pending || !scheduler.IsFree(c.tick)) {
        return;
    }
    c.pending = false;
    if (c.worker.joinable()) {
        c.worker.join();
    }
    if (!c.readback->is_coherent) {
        c.readback->Invalidate(0, c.plane_bytes * 2);
    }
    const u8* const data = c.readback->mapped_data.data();
    const u64 texels = u64{c.width} * c.height;
    c.busy.store(true, std::memory_order_relaxed);
    c.worker = std::jthread([data, plane = c.plane_bytes, texels, kind = c.kind, pass = c.pass,
                             frame = c.frame, records = c.records, fallbacks = c.fallbacks] {
        Diff(data, data + plane, texels, kind, pass, frame, records, fallbacks);
        g_state.compare.busy.store(false, std::memory_order_release);
    });
}

bool EnsureCompareImage(const Vulkan::Instance& instance, vk::Format format, u32 width, u32 height,
                        bool has_stencil) {
    Compare& c = g_state.compare;
    if (c.image && c.format == format && c.width == width && c.height == height) {
        return true;
    }
    c.view.reset();
    if (c.image) {
        c.image.Destroy();
    }
    c.image = VideoCore::UniqueImage(g_state.device, instance.GetAllocator());
    c.image.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    });
    const vk::ImageAspectFlags aspect =
        has_stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                    : vk::ImageAspectFlagBits::eDepth;
    auto [result, view] = g_state.device.createImageViewUnique({
        .image = c.image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = {aspect, 0, 1, 0, 1},
    });
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no compare image view: {}", vk::to_string(result));
        c.image.Destroy();
        return false;
    }
    c.view = std::move(view);
    c.format = format;
    c.width = width;
    c.height = height;
    c.has_stencil = has_stencil;
    c.kind = KindOf(format);
    c.plane_bytes =
        Common::AlignUp(u64{width} * height * (c.kind == DepthKind::Unorm16 ? 2 : 4), 256);
    if (!c.readback || c.readback->SizeBytes() < c.plane_bytes * 2) {
        c.readback = std::make_unique<VideoCore::Buffer>(instance, 0, c.plane_bytes * 2,
                                                         VideoCore::MemoryType::HostCached,
                                                         "GR2 PT caster compare");
    }
    return true;
}

vk::ImageAspectFlags Aspects(bool has_stencil) {
    return has_stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                       : vk::ImageAspectFlagBits::eDepth;
}

void ImageBarrier(Vulkan::Scheduler& scheduler, vk::Image image, bool has_stencil,
                  vk::ImageLayout from, vk::ImageLayout to, vk::PipelineStageFlags2 src_stage,
                  vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                  vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {Aspects(has_stencil), 0, 1, 0, 1},
    };
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    });
}

vk::ImageLayout AttachmentLayout(bool has_stencil) {
    return has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                       : vk::ImageLayout::eDepthAttachmentOptimal;
}

void Report() {
    Stats& s = g_state.stats;
    const CasterBatch::Counters counters = CasterBatch::Drain();
    const double frames = s.frames ? s.frames : 1;
    LOG_INFO(Render_Vulkan,
             "GR2 PT casters per frame over {} frames: {:.0f} drawn in {:.1f} indirect draws, "
             "{:.1f} drawn by the game's callback ({:.1f} unsupported, {:.1f} with the ring "
             "full), {:.1f} ranges obtained, {:.1f} dropped, {} kept, {:.0f} merged reads, "
             "{:.2f} barrier flushes, rendering restarts {:.2f} closed, {:.2f} after the game's "
             "clear, {:.2f} other, "
             "{:.2f} clears, {:.0f} KB of draw data, {:.2f} ring waits, {:.1f} GPU-written and "
             "{:.1f} misaligned matrices",
             s.frames, s.records / frames, s.groups / frames, s.fallbacks / frames,
             counters.unsupported / frames, counters.ring_full / frames, s.obtains / frames,
             s.dropped / frames, g_state.ranges.size(), s.reads / frames, s.flushes / frames,
             s.restarts_closed / frames, s.restarts_cleared / frames, s.restarts_other / frames,
             s.clears / frames, s.ring_bytes / frames / 1024.0, s.ring_waits / frames,
             s.gpu_matrices / frames, s.misaligned / frames);
    const double ms = frames * 1e6;
    LOG_INFO(Render_Vulkan,
             "GR2 PT caster batches per frame: {:.3f} ms resolving, {:.3f} ms finding the target, "
             "{:.3f} ms on barriers, {:.3f} ms beginning rendering, {:.3f} ms recording, {:.3f} "
             "ms finishing",
             s.resolve_ns / ms, s.target_ns / ms, s.barrier_ns / ms, s.begin_ns / ms,
             s.record_ns / ms, s.finish_ns / ms);
    s = {};
}

} // namespace

void Prepare(Vulkan::Rasterizer& rasterizer) {
    if (g_state.prepared) {
        Poll(rasterizer.GetScheduler());
        return;
    }
    g_state.prepared = true;
    const Vulkan::Instance& instance = RasterizerAccess::Instance(rasterizer);
    const auto features =
        instance.GetPhysicalDevice()
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                          vk::PhysicalDeviceVulkan12Features>();
    if (!features.get<vk::PhysicalDeviceFeatures2>().features.multiDrawIndirect ||
        !features.get<vk::PhysicalDeviceFeatures2>().features.shaderInt64 ||
        !features.get<vk::PhysicalDeviceVulkan11Features>().shaderDrawParameters ||
        !features.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress) {
        LOG_WARNING(Render_Vulkan, "GR2 PT: the device cannot pull vertices for the caster "
                                   "renderer, the game draws its casters");
        return;
    }
    g_state.device = instance.GetDevice();
    const vk::PushConstantRange constants{
        .stageFlags = vk::ShaderStageFlagBits::eVertex,
        .offset = 0,
        .size = sizeof(Constants),
    };
    auto [result, layout] = g_state.device.createPipelineLayout({
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &constants,
    });
    if (result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no caster pipeline layout: {}", vk::to_string(result));
        return;
    }
    g_state.layout = layout;
    g_state.static_module = Vulkan::CompileSPV(CASTER_STATIC_VERT, g_state.device);
    g_state.skinned_module = Vulkan::CompileSPV(CASTER_SKINNED_VERT, g_state.device);
    g_state.ring = std::make_unique<VideoCore::Buffer>(
        instance, 0, RingBytes, VideoCore::MemoryType::Stream, "GR2 PT casters");
    if (!g_state.static_module || !g_state.skinned_module || g_state.ring->mapped_data.empty()) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no caster shaders or draw data ring");
        return;
    }
    g_state.ring_address = g_state.ring->BufferDeviceAddress();
    g_state.ready = true;
    CasterBatch::Enable();
    LOG_INFO(Render_Vulkan, "GR2 PT: the caster renderer draws the opaque shadow casters");
}

void OnSnapshot(Vulkan::Rasterizer& rasterizer, const Marker::Words& words) {
    Compare& c = g_state.compare;
    c.snapshot = false;
    if (!g_state.ready || c.pending || c.busy.load(std::memory_order_acquire)) {
        return;
    }
    Target target;
    if (const char* reason = FindTarget(rasterizer, target)) {
        LOG_WARNING(Render_Vulkan, "GR2 PT: no compare copy: {}", reason);
        return;
    }
    VideoCore::Image& image = *target.image;
    if (target.range.extent.layers != 1) {
        LOG_WARNING(Render_Vulkan, "GR2 PT: no compare copy of a layered shadow map");
        return;
    }
    const vk::Format format = image.backing->image.image_ci.format;
    const u32 width = image.info.size.width;
    const u32 height = image.info.size.height;
    const Vulkan::Instance& instance = RasterizerAccess::Instance(rasterizer);
    if (!EnsureCompareImage(instance, format, width, height, target.has_stencil)) {
        return;
    }
    Vulkan::Scheduler& scheduler = rasterizer.GetScheduler();
    Vulkan::Runtime& runtime = RasterizerAccess::Barriers(rasterizer);
    scheduler.EndRendering();
    runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eCopy,
                    vk::AccessFlagBits2::eTransferRead, target.range);
    runtime.FlushBarriers();
    ImageBarrier(scheduler, c.image.image, c.has_stencil, vk::ImageLayout::eUndefined,
                 vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eAllCommands,
                 vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite);
    const vk::ImageCopy region{
        .srcSubresource = {Aspects(c.has_stencil), target.range.base.level, target.range.base.layer,
                           1},
        .dstSubresource = {Aspects(c.has_stencil), 0, 0, 1},
        .extent = {width, height, 1},
    };
    scheduler.CommandBuffer().copyImage(image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                        c.image.image, vk::ImageLayout::eTransferDstOptimal,
                                        region);
    ImageBarrier(scheduler, c.image.image, c.has_stencil, vk::ImageLayout::eTransferDstOptimal,
                 AttachmentLayout(c.has_stencil), vk::PipelineStageFlagBits2::eCopy,
                 vk::AccessFlagBits2::eTransferWrite, DepthStages, DepthAccess);
    c.snapshot = true;
    c.pass = words.pass;
    c.frame = words.frame;
    VideoCore::Skipcache::Framework::Instance().InvalidateAll();
}

void OnBatch(Vulkan::Rasterizer& rasterizer, const Marker::Words& words) {
    const u32 first = words.first;
    const u32 count = words.count;
    const bool compare = (words.flags & Marker::CompareBatch) != 0;
    Stats& stats = g_state.stats;
    if (words.pass == ShadowPass::FirstPass) {
        if (stats.frames == ReportFrames) {
            Report();
        }
        ++stats.frames;
    }
    ++stats.batches;
    stats.fallbacks += words.draws;
    if (!g_state.ready || count == 0 ||
        (compare && !(g_state.compare.snapshot && g_state.compare.pass == words.pass))) {
        CasterBatch::Release(first + count);
        return;
    }
    auto time = std::chrono::steady_clock::now();
    u64 base;
    const bool resolved = Resolve(rasterizer, first, count, base);
    CasterBatch::Release(first + count);
    stats.resolve_ns += Nanoseconds(time);
    if (!resolved) {
        Fail("a batch is larger than a quarter of the draw data ring");
        return;
    }
    Target target;
    if (const char* reason = FindTarget(rasterizer, target)) {
        Fail(reason);
        return;
    }
    const Vulkan::Instance& instance = RasterizerAccess::Instance(rasterizer);
    const Pipelines* pipelines = FindPipelines(instance, target.key);
    if (pipelines == nullptr) {
        Fail("no pipelines");
        return;
    }
    Vulkan::Scheduler& scheduler = rasterizer.GetScheduler();
    Vulkan::Runtime& runtime = RasterizerAccess::Barriers(rasterizer);
    stats.records += count;
    stats.target_ns += Nanoseconds(time);

    MergeReads();
    stats.reads += g_state.reads.size();
    bool needs_barrier = NeedsBarrier(runtime);
    Vulkan::RenderState state = target.state;
    Compare& c = g_state.compare;
    if (compare) {
        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *c.view;
        attachment.image_layout = AttachmentLayout(c.has_stencil);
        attachment.clear_value = {};
        attachment.depth_clear = false;
        attachment.stencil_clear = false;
        state.width = c.width;
        state.height = c.height;
        state.num_layers = 1;
    } else {
        needs_barrier |=
            runtime.Transit(target.image, target.layout, DepthStages, DepthAccess, target.range);
        state.depth_stencil_attachment.image_layout = target.image->backing->state.layout;
        stats.clears += state.depth_stencil_attachment.depth_clear;
    }
    if (needs_barrier) {
        runtime.FlushBarriers();
        ++stats.flushes;
    }
    stats.barrier_ns += Nanoseconds(time);
    CountRestart(scheduler, state);
    scheduler.BeginRendering(state);
    RasterizerAccess::UpdatePassState(rasterizer);
    scheduler.GetDynamicState().Commit(instance, scheduler);
    stats.begin_ns += Nanoseconds(time);
    RecordDraws(scheduler, *pipelines, base, count, target.flags);
    stats.record_ns += Nanoseconds(time);
    AccessReads(runtime);
    g_state.fences.emplace_back(g_state.batch_position, scheduler.CurrentTick());

    if (compare) {
        scheduler.EndRendering();
        runtime.Transit(target.image, vk::ImageLayout::eTransferSrcOptimal,
                        vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead,
                        target.range);
        runtime.FlushBarriers();
        ImageBarrier(scheduler, c.image.image, c.has_stencil, AttachmentLayout(c.has_stencil),
                     vk::ImageLayout::eTransferSrcOptimal, DepthStages, DepthAccess,
                     vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferRead);
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::BufferImageCopy game_copy{
            .bufferOffset = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eDepth, target.range.base.level,
                                 target.range.base.layer, 1},
            .imageExtent = {c.width, c.height, 1},
        };
        cmdbuf.copyImageToBuffer(target.image->GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                 c.readback->Handle(), game_copy);
        const vk::BufferImageCopy our_copy{
            .bufferOffset = c.plane_bytes,
            .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .imageExtent = {c.width, c.height, 1},
        };
        cmdbuf.copyImageToBuffer(c.image.image, vk::ImageLayout::eTransferSrcOptimal,
                                 c.readback->Handle(), our_copy);
        const vk::MemoryBarrier2 to_host{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{
            .memoryBarrierCount = 1,
            .pMemoryBarriers = &to_host,
        });
        c.snapshot = false;
        c.pending = true;
        c.tick = scheduler.CurrentTick();
        c.records = count;
        c.fallbacks = words.draws;
        VideoCore::Skipcache::Framework::Instance().InvalidateAll();
    }
    Restore(scheduler);
    stats.finish_ns += Nanoseconds(time);
}

} // namespace Gr2Pt::CasterRenderer
