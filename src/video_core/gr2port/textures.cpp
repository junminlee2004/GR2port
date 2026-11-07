// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/gr2port/hooks.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/textures.h"
#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/tile.h"

namespace Gr2Port::Textures {

namespace {

// InitTextureFromGnf FUN_01618ca0(texture, gnf, srgb) fills a texture object from a GNF blob that
// it keeps; DestroyTexture FUN_016182a0(texture) frees the blob.
constexpr Hooks::Entry Init{0x1618ca0, 6, Hooks::FramePrologue};
constexpr Hooks::Entry Destroy{0x16182a0, 6, Hooks::FramePrologue};

// Texture object: T# at +0x30, the renderer's handle at +0x6c, the GNF blob at +0x70.
constexpr u64 TextureSharp = 0x30;
constexpr u64 TextureHandle = 0x6c;
constexpr u64 TextureBlob = 0x70;
// GNF blob: a header {magic, contents size}, then the contents, whose stream size is at +4; the
// texture data stream follows the contents.
constexpr u64 BlobContentsSize = 0x4;
constexpr u64 BlobStreamSize = 0xc;
constexpr u64 BlobContents = 0x8;

// The tilings the detile shaders convert, and the bits per pixel or block of each shader.
constexpr std::array<AmdGpu::TileMode, 4> TileModes{
    AmdGpu::TileMode::Display1DThin,
    AmdGpu::TileMode::Display2DThin,
    AmdGpu::TileMode::Thin1DThin,
    AmdGpu::TileMode::Thin2DThin,
};
constexpr std::array<u32, 5> BlockBits{8, 16, 32, 64, 128};

constexpr u64 UploadBudget = 64_MB;
// Beside the emulated GPU's renderer, which keeps its own copy of every texture, a device with a
// budget below SmallDeviceBudget holds only part of them: images start at the first level no larger
// than SmallDeviceExtent. Other devices hold every level: LargeDeviceExtent exceeds any image.
constexpr u64 SmallDeviceBudget = 6_GB;
constexpr u32 SmallDeviceExtent = 512;
constexpr u32 LargeDeviceExtent = 1u << 16;
constexpr u32 LoggedTextures = 8;
// The detile parameters lead each staging buffer; the texture data follows.
constexpr u64 ParamsBytes = 256;

// The detile shaders' parameters (host_shaders/tiling.comp). For block-compressed formats the
// pitch and height count blocks.
struct TilingParams {
    u32 bank_swizzle;
    u32 micro_tiled_mips;
    u32 num_mips;
    std::array<VideoCore::MipInfo, VideoCore::MAX_MIPS> mips;
};
static_assert(sizeof(TilingParams) <= ParamsBytes);

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

struct Texture {
    std::atomic<VAddr> object{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> cube{false};
};

// What a texture object holds when it is built: its T# and the end of its GNF data.
struct Source {
    AmdGpu::Image sharp;
    VAddr end;
};

std::array<Texture, Capacity> g_textures;
std::array<Source, Capacity> g_sources;

// Guards the handle lists, the sources and the reads of texture data that destroys free.
std::mutex g_mutex;
std::vector<u32> g_free;
u32 g_unused = 1;
std::deque<u32> g_pending;
std::vector<u32> g_destroyed;
u32 g_not_2d = 0;
u32 g_unsupported = 0;
u32 g_missed_destroys = 0;
u32 g_logged_skips = 0;

// Render thread only.
struct Layout {
    vk::Format format;
    vk::ComponentMapping mapping;
    // The size of the data's first level, and of the image's.
    vk::Extent2D extent;
    vk::Extent2D image_extent;
    // The levels of the data; the data level the image starts at, its offset and the bytes from
    // it on; the image's levels (at most a full chain) and layers.
    u32 data_levels;
    u32 first;
    u32 base;
    u32 bytes;
    u32 levels;
    u32 layers;
    u32 bits;
    bool block;
    AmdGpu::TileMode tile_mode;
    u32 size;
    u32 micro_tiled_mips;
    std::array<VideoCore::MipInfo, VideoCore::MAX_MIPS> mips;
};
struct Retiring {
    u32 handle;
    // The last frame that can sample the texture.
    u64 frame;
};
struct Staging {
    Buffer buffer;
    u64 frame;
};
struct Upload {
    u32 handle;
    Image image;
    Buffer source;
    u64 source_offset;
    const Layout* layout;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
vk::Sampler g_sampler;
vk::Sampler g_clamp_sampler;
vk::DescriptorSetLayout g_set_layout;
vk::DescriptorPool g_pool;
vk::DescriptorSet g_set;
Image g_white;
bool g_white_cleared = false;
vk::DescriptorSetLayout g_detile_set_layout;
vk::PipelineLayout g_detile_layout;
// Indexed by the position in TileModes and in BlockBits.
std::array<std::array<vk::Pipeline, BlockBits.size()>, TileModes.size()> g_detile{};
std::array<Image, Capacity> g_resident;
std::vector<Retiring> g_retiring;
std::vector<Staging> g_staging;
std::unordered_map<VkFormat, bool> g_format_support;
u32 g_logged_uploads = 0;
Stats g_stats{};

namespace LiverpoolToVK = Vulkan::LiverpoolToVK;

// The table lookup of LiverpoolToVK::SurfaceFormat, which asserts on unknown formats.
vk::Format SurfaceFormat(const AmdGpu::Image& sharp) {
    return LiverpoolToVK::surface_format_table[LiverpoolToVK::GetSurfaceFormatTableIndex(
        sharp.GetDataFmt(), sharp.GetNumberFmt())];
}

bool ValidSwizzle(AmdGpu::CompSwizzle swizzle) {
    const u32 raw = static_cast<u32>(swizzle);
    return raw <= 1 || (raw >= 4 && raw <= 7);
}

bool Tiled(AmdGpu::TileMode tile_mode) {
    return tile_mode != AmdGpu::TileMode::DisplayLinearAligned;
}

bool IsCube(const AmdGpu::Image& sharp) {
    return sharp.GetType() == AmdGpu::ImageType::Cube;
}

// The checks on a T# that need no layout: a single 2D image or a single cube, with a tiling and
// format the renderer converts.
bool SingleImage(const AmdGpu::Image& sharp) {
    const bool single =
        IsCube(sharp) ? sharp.depth == 0
                      : sharp.GetType() == AmdGpu::ImageType::Color2D && sharp.NumLayers() == 1;
    return single && sharp.base_level == 0 && sharp.alt_tile_mode == 0 && sharp.Address() != 0;
}

bool Convertible(const AmdGpu::Image& sharp) {
    const AmdGpu::TileMode tile_mode = sharp.GetTileMode();
    if (Tiled(tile_mode) && std::ranges::find(TileModes, tile_mode) == TileModes.end()) {
        return false;
    }
    const u32 data_format = static_cast<u32>(sharp.GetDataFmt());
    const u32 number_format = static_cast<u32>(sharp.GetNumberFmt());
    if (data_format >= 64 || number_format >= 16) {
        return false;
    }
    const auto format = SurfaceFormat(sharp);
    const AmdGpu::CompMapping select = sharp.DstSelect();
    return format != vk::Format::eUndefined &&
           std::ranges::find(BlockBits, AmdGpu::NumBitsPerBlock(sharp.GetDataFmt())) !=
               BlockBits.end() &&
           ValidSwizzle(select.r) && ValidSwizzle(select.g) && ValidSwizzle(select.b) &&
           ValidSwizzle(select.a);
}

Layout ReadLayout(const AmdGpu::Image& sharp, u32 max_extent) {
    Layout layout{};
    layout.format = SurfaceFormat(sharp);
    layout.mapping = LiverpoolToVK::ComponentMapping(sharp.DstSelect());
    layout.extent = {static_cast<u32>(sharp.width) + 1, static_cast<u32>(sharp.height) + 1};
    layout.data_levels = sharp.NumLevels();
    while (layout.first + 1 < layout.data_levels &&
           std::max(layout.extent.width, layout.extent.height) >> layout.first > max_extent) {
        layout.first++;
    }
    layout.image_extent = {std::max(layout.extent.width >> layout.first, 1u),
                           std::max(layout.extent.height >> layout.first, 1u)};
    layout.levels = std::min<u32>(
        layout.data_levels - layout.first,
        std::bit_width(std::max(layout.image_extent.width, layout.image_extent.height)));
    layout.layers = IsCube(sharp) ? 6 : 1;
    layout.bits = AmdGpu::NumBitsPerBlock(sharp.GetDataFmt());
    layout.block = AmdGpu::IsBlockCoded(sharp.GetDataFmt());
    layout.tile_mode = sharp.GetTileMode();
    // The data can hold more layers than the image: padding rounds their count to a power of 2.
    VideoCore::ComputeImageSize(sharp.Pitch(), layout.extent.height, 1, layout.data_levels,
                                sharp.NumLayers(), layout.bits, 1, layout.block, sharp.pow2pad != 0,
                                false, layout.tile_mode, AmdGpu::GetArrayMode(layout.tile_mode),
                                &layout.size, &layout.micro_tiled_mips, layout.mips.data());
    layout.base = layout.mips[layout.first].offset;
    layout.bytes = layout.size - layout.base;
    return layout;
}

void LogSkip(VAddr texture, const AmdGpu::Image& sharp, const char* reason) {
    if (g_logged_skips >= LoggedTextures) {
        return;
    }
    g_logged_skips++;
    LOG_INFO(Render_Vulkan,
             "GR2port: texture {:#x} not converted ({}): type {}, {}x{}, levels {}, layers {}, "
             "tile mode {}, data format {}, number format {}",
             texture, reason, static_cast<u32>(sharp.GetType()), sharp.width + 1, sharp.height + 1,
             sharp.NumLevels(), sharp.NumLayers(), static_cast<u32>(sharp.GetTileMode()),
             static_cast<u32>(sharp.GetDataFmt()), static_cast<u32>(sharp.GetNumberFmt()));
}

void PS4_SYSV_ABI OnInit(u32, const Hooks::Arguments* arguments, u64) {
    const VAddr texture = arguments->rdi;
    if (texture == 0) {
        return;
    }
    AmdGpu::Image sharp;
    std::memcpy(&sharp, reinterpret_cast<const void*>(texture + TextureSharp), sizeof(sharp));
    const VAddr blob = Load<VAddr>(texture + TextureBlob);
    std::scoped_lock lock{g_mutex};
    // The handle field starts as heap contents, which can be the handle of an earlier texture
    // at this address.
    const u32 earlier = Load<u32>(texture + TextureHandle);
    if (earlier != 0 && earlier < Capacity &&
        g_textures[earlier].object.load(std::memory_order_relaxed) == texture) {
        g_textures[earlier].ready.store(false, std::memory_order_relaxed);
        g_textures[earlier].object.store(0, std::memory_order_release);
        g_destroyed.push_back(earlier);
        g_missed_destroys++;
    }
    u32 handle = 0;
    if (!SingleImage(sharp)) {
        g_not_2d++;
        LogSkip(texture, sharp, "not a single 2D image or cube");
    } else if (blob == 0 || !Convertible(sharp)) {
        g_unsupported++;
        LogSkip(texture, sharp, "tiling or format");
    } else if (!g_free.empty()) {
        handle = g_free.back();
        g_free.pop_back();
    } else if (g_unused < Capacity) {
        handle = g_unused++;
    }
    std::memcpy(reinterpret_cast<void*>(texture + TextureHandle), &handle, sizeof(handle));
    if (handle == 0) {
        return;
    }
    const VAddr stream = blob + BlobContents + Load<u32>(blob + BlobContentsSize);
    g_sources[handle] = {.sharp = sharp, .end = stream + Load<u32>(blob + BlobStreamSize)};
    g_textures[handle].ready.store(false, std::memory_order_relaxed);
    g_textures[handle].cube.store(IsCube(sharp), std::memory_order_relaxed);
    g_textures[handle].object.store(texture, std::memory_order_release);
    g_pending.push_back(handle);
}

void PS4_SYSV_ABI OnDestroy(u32, const Hooks::Registers* registers, u64) {
    const VAddr texture = registers->rdi;
    if (texture == 0) {
        return;
    }
    const u32 handle = Load<u32>(texture + TextureHandle);
    if (handle == 0 || handle >= Capacity ||
        g_textures[handle].object.load(std::memory_order_relaxed) != texture) {
        return;
    }
    std::scoped_lock lock{g_mutex};
    if (g_textures[handle].object.load(std::memory_order_relaxed) != texture) {
        return;
    }
    g_textures[handle].ready.store(false, std::memory_order_relaxed);
    g_textures[handle].object.store(0, std::memory_order_release);
    g_destroyed.push_back(handle);
}

void WriteDescriptor(u32 handle, vk::ImageView view) {
    const vk::DescriptorImageInfo info{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = g_set,
        .dstBinding = 0,
        .dstArrayElement = handle,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
}

void Retire(u64 frame, u64 completed) {
    std::vector<u32> destroyed;
    {
        std::scoped_lock lock{g_mutex};
        destroyed.swap(g_destroyed);
    }
    // The packet of this frame, or the one the capture was writing at the destroy, which the next
    // frame takes at the latest, can still sample the texture.
    for (const u32 handle : destroyed) {
        g_retiring.push_back({handle, frame + 1});
    }
    std::erase_if(g_retiring, [completed](const Retiring& r) {
        if (completed < r.frame) {
            return false;
        }
        Image& image = g_resident[r.handle];
        if (image.image) {
            WriteDescriptor(r.handle, g_white.view);
            g_stats.resident_bytes -= image.size;
            g_memory->Destroy(image);
            g_stats.textures--;
        }
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

bool FormatSupported(vk::Format format) {
    const auto [it, inserted] = g_format_support.try_emplace(static_cast<VkFormat>(format), false);
    if (inserted) {
        const auto features = g_device->gpu.getFormatProperties(format, g_device->dispatch);
        const auto needed =
            vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eTransferDst;
        it->second = (features.optimalTilingFeatures & needed) == needed;
    }
    return it->second;
}

vk::Pipeline DetilePipeline(const Layout& layout) {
    const auto mode = std::ranges::find(TileModes, layout.tile_mode);
    const auto bits = std::ranges::find(BlockBits, layout.bits);
    return g_detile[mode - TileModes.begin()][bits - BlockBits.begin()];
}

// Records the detile of the staging buffer's texture data into scratch.
void RecordDetile(vk::CommandBuffer cmd, const Layout& layout, Buffer& staging, Buffer& scratch,
                  u32 bank_swizzle) {
    TilingParams params{
        .bank_swizzle = bank_swizzle,
        .micro_tiled_mips = layout.micro_tiled_mips >> layout.first,
        .num_mips = layout.data_levels - layout.first,
    };
    // The staging buffer holds the data from the first converted level on.
    for (u32 mip = 0; mip < params.num_mips; mip++) {
        params.mips[mip] = layout.mips[layout.first + mip];
        params.mips[mip].offset -= layout.base;
        if (layout.block) {
            params.mips[mip].pitch = std::max<u16>((params.mips[mip].pitch + 3) / 4, 1);
            params.mips[mip].height = std::max<u16>((params.mips[mip].height + 3) / 4, 1);
        }
    }
    std::memcpy(staging.mapped, &params, sizeof(params));
    const vk::DescriptorBufferInfo tiled{staging.buffer, ParamsBytes, layout.bytes};
    const vk::DescriptorBufferInfo linear{scratch.buffer, 0, layout.bytes};
    const vk::DescriptorBufferInfo uniform{staging.buffer, 0, sizeof(params)};
    const std::array<vk::WriteDescriptorSet, 3> writes{{
        {.dstBinding = 0,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &tiled},
        {.dstBinding = 1,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer,
         .pBufferInfo = &linear},
        {.dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eUniformBuffer,
         .pBufferInfo = &uniform},
    }};
    const auto& dl = g_device->dispatch;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, DetilePipeline(layout), dl);
    cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, g_detile_layout, 0, writes, dl);
    // One invocation per pixel or block, 64 per group; tiled sizes are whole 8x8 tiles.
    cmd.dispatch(layout.bytes / (layout.bits / 8) / 64, 1, 1, dl);
}

bool CreateDetile(const Device& d) {
    const auto& dl = d.dispatch;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eCompute},
    }};
    g_detile_set_layout =
        d.device
            .createDescriptorSetLayout(
                {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                 .bindingCount = static_cast<u32>(bindings.size()),
                 .pBindings = bindings.data()},
                nullptr, dl)
            .value;
    g_detile_layout =
        d.device
            .createPipelineLayout({.setLayoutCount = 1, .pSetLayouts = &g_detile_set_layout},
                                  nullptr, dl)
            .value;
    if (!g_detile_set_layout || !g_detile_layout) {
        return false;
    }
    for (size_t m = 0; m < TileModes.size(); m++) {
        const AmdGpu::TileMode tile_mode = TileModes[m];
        const AmdGpu::ArrayMode array_mode = AmdGpu::GetArrayMode(tile_mode);
        const auto micro_tile_mode = AmdGpu::GetMicroTileMode(tile_mode);
        const bool macro = AmdGpu::IsMacroTiled(array_mode);
        for (size_t b = 0; b < BlockBits.size(); b++) {
            const u32 bits = BlockBits[b];
            // The emulator's specialisation of host_shaders/tiling.comp for one sample, detiling.
            std::array<u32, 12> constants{1, static_cast<u32>(micro_tile_mode),
                                          AmdGpu::GetMicroTileThickness(array_mode), 0};
            if (macro) {
                const auto macro_tile_mode = AmdGpu::CalculateMacrotileMode(tile_mode, bits, 1);
                constants[4] = static_cast<u32>(array_mode);
                constants[5] = static_cast<u32>(AmdGpu::GetPipeConfig(tile_mode));
                constants[6] = AmdGpu::GetBankWidth(macro_tile_mode);
                constants[7] = AmdGpu::GetBankHeight(macro_tile_mode);
                constants[8] = AmdGpu::GetNumBanks(macro_tile_mode);
                constants[9] = std::bit_width(constants[8]) - 1;
                constants[10] =
                    AmdGpu::CalculateTileSplit(tile_mode, array_mode, micro_tile_mode, bits);
                constants[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
            }
            std::span<const u32> code;
            switch (bits) {
            case 8:
                code = macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                             : std::span<const u32>{TILING_MICRO_8_COMP};
                break;
            case 16:
                code = macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                             : std::span<const u32>{TILING_MICRO_16_COMP};
                break;
            case 32:
                code = macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                             : std::span<const u32>{TILING_MICRO_32_COMP};
                break;
            case 64:
                code = macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                             : std::span<const u32>{TILING_MICRO_64_COMP};
                break;
            default:
                code = macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                             : std::span<const u32>{TILING_MICRO_128_COMP};
                break;
            }
            std::array<vk::SpecializationMapEntry, 12> entries{};
            for (u32 i = 0; i < entries.size(); i++) {
                entries[i] = {i, i * static_cast<u32>(sizeof(u32)), sizeof(u32)};
            }
            const u32 count = macro ? 12 : 4;
            const vk::SpecializationInfo specialization{
                .mapEntryCount = count,
                .pMapEntries = entries.data(),
                .dataSize = count * sizeof(u32),
                .pData = constants.data(),
            };
            const vk::ShaderModule module =
                d.device
                    .createShaderModule({.codeSize = code.size_bytes(), .pCode = code.data()},
                                        nullptr, dl)
                    .value;
            const vk::ComputePipelineCreateInfo info{
                .stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                          .module = module,
                          .pName = "main",
                          .pSpecializationInfo = &specialization},
                .layout = g_detile_layout,
            };
            const auto [result, pipeline] = d.device.createComputePipeline({}, info, nullptr, dl);
            d.device.destroyShaderModule(module, nullptr, dl);
            if (result != vk::Result::eSuccess) {
                LOG_ERROR(Render_Vulkan, "GR2port: no detile pipeline for tile mode {}, {} bits",
                          static_cast<u32>(tile_mode), bits);
                return false;
            }
            g_detile[m][b] = pipeline;
        }
    }
    return true;
}

bool CreateSet(const Device& d) {
    const auto& dl = d.dispatch;
    const auto properties =
        d.gpu.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan12Properties>(
            dl);
    const auto& limits = properties.get<vk::PhysicalDeviceVulkan12Properties>();
    if (limits.maxPerStageDescriptorUpdateAfterBindSampledImages < Capacity ||
        limits.maxDescriptorSetUpdateAfterBindSampledImages < Capacity) {
        LOG_ERROR(Render_Vulkan, "GR2port: the GPU binds fewer than {} textures", Capacity);
        return false;
    }
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eLinear,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eRepeat,
        .addressModeW = vk::SamplerAddressMode::eRepeat,
        // The game's loader sets this bias and ratio on every texture it loads.
        .mipLodBias = -51.0f / 256.0f,
        .anisotropyEnable = true,
        .maxAnisotropy = 2.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    g_sampler = d.device.createSampler(sampler_info, nullptr, dl).value;
    // The game clamps its decal textures and cube maps to their edges.
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    g_clamp_sampler = d.device.createSampler(sampler_info, nullptr, dl).value;
    constexpr vk::ShaderStageFlags Stages = vk::ShaderStageFlagBits::eVertex |
                                            vk::ShaderStageFlagBits::eFragment |
                                            vk::ShaderStageFlagBits::eCompute;
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eSampledImage, Capacity, Stages},
        {1, vk::DescriptorType::eSampler, 1, Stages, &g_sampler},
        {2, vk::DescriptorType::eSampler, 1, Stages, &g_clamp_sampler},
    }};
    const std::array<vk::DescriptorBindingFlags, 3> binding_flags{
        vk::DescriptorBindingFlagBits::ePartiallyBound |
            vk::DescriptorBindingFlagBits::eUpdateAfterBind |
            vk::DescriptorBindingFlagBits::eUpdateUnusedWhilePending,
        {},
        {},
    };
    const vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        .bindingCount = static_cast<u32>(binding_flags.size()),
        .pBindingFlags = binding_flags.data(),
    };
    g_set_layout = d.device
                       .createDescriptorSetLayout(
                           {.pNext = &flags_info,
                            .flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool,
                            .bindingCount = static_cast<u32>(bindings.size()),
                            .pBindings = bindings.data()},
                           nullptr, dl)
                       .value;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {vk::DescriptorType::eSampledImage, Capacity},
        {vk::DescriptorType::eSampler, 2},
    }};
    g_pool = d.device
                 .createDescriptorPool({.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
                                        .maxSets = 1,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, dl)
                 .value;
    if (!g_sampler || !g_clamp_sampler || !g_set_layout || !g_pool) {
        return false;
    }
    const auto [result, sets] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_set_layout}, dl);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    g_set = sets.front();
    return true;
}

// Clears the white texture once, in the first frame.
void ClearWhite(vk::CommandBuffer cmd) {
    const auto& dl = g_device->dispatch;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::ImageMemoryBarrier2 barrier{
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = g_white.image,
        .subresourceRange = range,
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dl);
    cmd.clearColorImage(
        g_white.image, vk::ImageLayout::eTransferDstOptimal,
        vk::ClearColorValue{.float32 = std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}}, range, dl);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eClear;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask =
        vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dl);
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    const Hooks::Result init = Hooks::ObserveReturn(module_base, Init, c, capacity, &OnInit, 0);
    const Hooks::Result destroy =
        Hooks::ObserveEntry(module_base, Destroy, c, capacity, &OnDestroy, 0);
    const auto ok = [](Hooks::Result r) {
        return r == Hooks::Result::Hooked || r == Hooks::Result::Chained;
    };
    if (!ok(init) || !ok(destroy)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the texture init or destroy is not the expected code");
        return false;
    }
    return true;
}

namespace {

u32 GetOfKind(VAddr texture, bool cube) {
    const u32 handle = Load<u32>(texture + TextureHandle);
    if (handle == 0 || handle >= Capacity) {
        return 0;
    }
    const Texture& t = g_textures[handle];
    if (!t.ready.load(std::memory_order_acquire) ||
        t.object.load(std::memory_order_relaxed) != texture ||
        t.cube.load(std::memory_order_relaxed) != cube) {
        return 0;
    }
    return handle;
}

} // namespace

u32 Get(VAddr texture) {
    return GetOfKind(texture, false);
}

u32 GetCube(VAddr texture) {
    return GetOfKind(texture, true);
}

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    if (!CreateSet(device) || !CreateDetile(device)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the texture array or the detile pipelines failed");
        return false;
    }
    g_white = memory.CreateTexture(vk::Format::eR8G8B8A8Unorm, {1, 1}, 1, {});
    if (!g_white.image) {
        return false;
    }
    WriteDescriptor(0, g_white.view);
    return true;
}

void Update(vk::CommandBuffer cmd, u64 frame, u64 completed) {
    const auto& dl = g_device->dispatch;
    if (!g_white_cleared) {
        g_white_cleared = true;
        ClearWhite(cmd);
    }
    Retire(frame, completed);

    std::vector<Upload> uploads;
    // Layouts live until the copies are recorded, at the end of this call.
    std::deque<Layout> layouts;
    u64 budget = UploadBudget;
    const u32 max_extent =
        g_memory->DeviceBudget().budget < SmallDeviceBudget ? SmallDeviceExtent : LargeDeviceExtent;
    {
        std::scoped_lock lock{g_mutex};
        while (!g_pending.empty() && budget != 0) {
            const u32 handle = g_pending.front();
            g_pending.pop_front();
            const VAddr object = g_textures[handle].object.load(std::memory_order_relaxed);
            // A handle freed and reused before its first upload is queued twice.
            if (object == 0 || g_resident[handle].image) {
                continue;
            }
            const Source& source = g_sources[handle];
            const Layout& layout = layouts.emplace_back(ReadLayout(source.sharp, max_extent));
            const VAddr data = source.sharp.Address();
            if (data + layout.size > source.end || layout.size == 0) {
                g_stats.too_large++;
                if (g_logged_skips < LoggedTextures) {
                    g_logged_skips++;
                    LOG_INFO(Render_Vulkan,
                             "GR2port: texture {:#x} not converted: a layout of {} bytes, {} in "
                             "the GNF data",
                             object, layout.size, source.end - std::min(data, source.end));
                }
                continue;
            }
            if (!FormatSupported(layout.format)) {
                g_stats.failed++;
                continue;
            }
            // The image and the detile output.
            if (!g_memory->Reserve(2ull * layout.bytes)) {
                g_pending.push_front(handle);
                break;
            }
            const bool tiled = Tiled(layout.tile_mode);
            Staging staging{
                .buffer = g_memory->CreateBuffer(ParamsBytes + layout.bytes,
                                                 vk::BufferUsageFlagBits::eStorageBuffer |
                                                     vk::BufferUsageFlagBits::eUniformBuffer |
                                                     vk::BufferUsageFlagBits::eTransferSrc,
                                                 Placement::Staging),
                .frame = frame,
            };
            Staging scratch{.frame = frame};
            if (tiled) {
                scratch.buffer = g_memory->CreateBuffer(layout.bytes,
                                                        vk::BufferUsageFlagBits::eStorageBuffer |
                                                            vk::BufferUsageFlagBits::eTransferSrc,
                                                        Placement::Device);
            }
            Image image = g_memory->CreateTexture(layout.format, layout.image_extent, layout.levels,
                                                  layout.mapping, layout.layers != 1);
            if (staging.buffer.mapped == nullptr || (tiled && !scratch.buffer.buffer) ||
                !image.image) {
                g_memory->Destroy(staging.buffer);
                g_memory->Destroy(scratch.buffer);
                g_memory->Destroy(image);
                g_stats.failed++;
                continue;
            }
            std::memcpy(staging.buffer.mapped + ParamsBytes,
                        reinterpret_cast<const void*>(data + layout.base), layout.bytes);
            if (tiled) {
                RecordDetile(cmd, layout, staging.buffer, scratch.buffer,
                             source.sharp.GetBankSwizzle());
            }
            if (g_logged_uploads < LoggedTextures) {
                g_logged_uploads++;
                LOG_INFO(Render_Vulkan,
                         "GR2port: texture {:#x}: {}x{} {}, {} levels from level {}, tile mode "
                         "{}, {} of {} bytes",
                         object, layout.image_extent.width, layout.image_extent.height,
                         vk::to_string(layout.format), layout.levels, layout.first,
                         static_cast<u32>(layout.tile_mode), layout.bytes, layout.size);
            }
            uploads.push_back({
                .handle = handle,
                .image = image,
                .source = tiled ? scratch.buffer : staging.buffer,
                .source_offset = tiled ? 0 : ParamsBytes,
                .layout = &layout,
            });
            g_staging.push_back(staging);
            if (tiled) {
                g_staging.push_back(scratch);
            }
            g_resident[handle] = image;
            g_stats.textures++;
            g_stats.uploaded_bytes += layout.bytes;
            g_stats.uploaded_data_bytes += layout.size;
            g_stats.resident_bytes += image.size;
            budget = layout.bytes >= budget ? 0 : budget - layout.bytes;
        }
        g_stats.pending = static_cast<u32>(g_pending.size());
        g_stats.not_2d = g_not_2d;
        g_stats.unsupported = g_unsupported;
        g_stats.missed_destroys = g_missed_destroys;
    }
    if (uploads.empty()) {
        return;
    }

    std::vector<vk::ImageMemoryBarrier2> barriers;
    barriers.reserve(uploads.size());
    for (const Upload& upload : uploads) {
        barriers.push_back({
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = upload.image.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, upload.layout->levels, 0,
                                 upload.layout->layers},
        });
    }
    const vk::MemoryBarrier2 detiled{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1,
                          .pMemoryBarriers = &detiled,
                          .imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         dl);

    std::vector<vk::BufferImageCopy> copies;
    for (const Upload& upload : uploads) {
        const Layout& layout = *upload.layout;
        copies.clear();
        for (u32 mip = 0; mip < layout.levels; mip++) {
            const u32 level = layout.first + mip;
            const VideoCore::MipInfo& info = layout.mips[level];
            const u32 width = std::max(layout.extent.width >> level, 1u);
            const u32 height = std::max(layout.extent.height >> level, 1u);
            copies.push_back({
                .bufferOffset = upload.source_offset + info.offset - layout.base,
                .bufferRowLength = info.pitch,
                .bufferImageHeight = info.height,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, layout.layers},
                .imageExtent = {info.pitch != 0 ? std::min<u32>(info.pitch, width) : width,
                                info.height != 0 ? std::min<u32>(info.height, height) : height, 1},
            });
        }
        cmd.copyBufferToImage(upload.source.buffer, upload.image.image,
                              vk::ImageLayout::eTransferDstOptimal, copies, dl);
    }

    for (vk::ImageMemoryBarrier2& barrier : barriers) {
        barrier.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
        barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        barrier.dstStageMask =
            vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader;
        barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         dl);
    for (const Upload& upload : uploads) {
        WriteDescriptor(upload.handle, upload.image.view);
        g_textures[upload.handle].ready.store(true, std::memory_order_release);
    }
}

vk::DescriptorSetLayout SetLayout() {
    return g_set_layout;
}

vk::DescriptorSet Set() {
    return g_set;
}

Stats TakeStats() {
    Stats stats = g_stats;
    g_stats.uploaded_bytes = 0;
    g_stats.uploaded_data_bytes = 0;
    return stats;
}

} // namespace Gr2Port::Textures
