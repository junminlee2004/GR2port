// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "common/logging/log.h"
#include "video_core/gr2port/fog.h"
#include "video_core/gr2port/memory.h"
#include "video_core/gr2port/pass_times.h"
#include "video_core/gr2port/pipeline.h"
#include "video_core/gr2port/settings.h"
#include "video_core/gr2port/shaders/fog_apply_frag.h"
#include "video_core/gr2port/shaders/fog_bake_comp.h"
#include "video_core/gr2port/shaders/fog_darkness_comp.h"
#include "video_core/gr2port/shaders/fog_density_comp.h"
#include "video_core/gr2port/shaders/fog_scatter_comp.h"
#include "video_core/gr2port/shaders/fog_shadow_comp.h"
#include "video_core/gr2port/shaders/fullscreen_vert.h"
#include "video_core/gr2port/shadows.h"
#include "video_core/gr2port/textures.h"

namespace Gr2Port::Fog {

namespace {

// The game's fog cells, cloud maps and noise volumes.
// The fog's cells by fog quality: the game's 160 x 90 x 128, then 120 x 68 x 96 and 80 x 45 x 64.
constexpr std::array<vk::Extent3D, 3> CellExtents{
    vk::Extent3D{160, 90, 128}, vk::Extent3D{120, 68, 96}, vk::Extent3D{80, 45, 64}};
vk::Extent3D g_cells_extent = CellExtents[0];
constexpr vk::Extent3D MapExtent{128, 128, 32};
constexpr vk::Extent3D NoiseExtent{64, 64, 64};
constexpr u64 NoiseBytes = 64 * 64 * 64;
constexpr vk::Format CellFormat = vk::Format::eR8Unorm;
constexpr vk::Format ScatterFormat = vk::Format::eR16G16B16A16Sfloat;
// The stratus and region maps; the Perlin and fbm noise.
constexpr u32 Layers = 2;
constexpr u32 Noises = 2;
constexpr u32 PerlinNoise = 0;
constexpr u32 FbmNoise = 1;
constexpr u32 GroupSize = 8;
constexpr u32 ScatterGroupSize = 32;

struct FogPush {
    u64 fog;
};

struct BakePush {
    u64 fog;
    u32 layer;
    u32 padding;
};

struct CellPush {
    u64 fog;
    u64 light;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
bool g_enabled = false;
vk::Sampler g_clamp_sampler;
vk::Sampler g_wrap_sampler;
vk::Sampler g_map_sampler;
vk::Sampler g_nearest_sampler;
// Two of each: one the frame writes, the other the last frame's.
std::array<Image, 2> g_density;
std::array<Image, 2> g_darkness;
std::array<Image, 2> g_shadow;
Image g_scattered;
std::array<Image, Layers> g_maps;
std::array<Image, Layers> g_map_darkness;
std::array<Image, Noises> g_noise;
Buffer g_staging;
// The guest addresses the noise volumes came from; 0 until copied.
std::array<u64, Noises> g_noise_source{};
bool g_cleared = false;
u32 g_cells = 0;
u32 g_shadows = 0;
// The header's counts of map changes when the maps were last made.
std::array<u32, Layers> g_maps_made{};
std::array<u32, Layers> g_darkness_made{};
bool g_maps_stale = true;
vk::DescriptorSetLayout g_bake_layout;
vk::DescriptorSetLayout g_darkness_layout;
vk::DescriptorSetLayout g_cell_layout;
vk::DescriptorSetLayout g_shadow_layout;
vk::DescriptorSetLayout g_scatter_layout;
vk::DescriptorSetLayout g_apply_layout;
vk::DescriptorPool g_pool;
// Bakes by map; cells and shadows by the image they write; the scattering by the cells' and the
// shadows' images.
std::array<vk::DescriptorSet, Layers> g_bake_sets;
std::array<vk::DescriptorSet, Layers> g_darkness_sets;
std::array<vk::DescriptorSet, 2> g_cell_sets;
std::array<vk::DescriptorSet, 2> g_shadow_sets;
std::array<std::array<vk::DescriptorSet, 2>, 2> g_scatter_sets;
vk::DescriptorSet g_apply_set;
Pipeline g_bake;
Pipeline g_darkness_pipeline;
Pipeline g_cells_pipeline;
Pipeline g_shadow_pipeline;
Pipeline g_scatter;
Pipeline g_apply;

constexpr auto Combined = vk::DescriptorType::eCombinedImageSampler;
constexpr auto Storage = vk::DescriptorType::eStorageImage;

vk::DescriptorSetLayout CreateLayout(std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    return g_device->device
        .createDescriptorSetLayout(
            {.bindingCount = static_cast<u32>(bindings.size()), .pBindings = bindings.data()},
            nullptr, g_device->dispatch)
        .value;
}

vk::Sampler CreateSampler(vk::Filter filter, vk::SamplerAddressMode xy, vk::SamplerAddressMode z) {
    const vk::SamplerCreateInfo info{
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = xy,
        .addressModeV = xy,
        .addressModeW = z,
        .borderColor = vk::BorderColor::eFloatTransparentBlack,
    };
    return g_device->device.createSampler(info, nullptr, g_device->dispatch).value;
}

void Write(vk::DescriptorSet set, u32 binding, u32 element, vk::DescriptorType type,
           const Image& image, vk::Sampler sampler,
           vk::ImageLayout layout = vk::ImageLayout::eGeneral) {
    const vk::DescriptorImageInfo info{sampler, image.view, layout};
    const vk::WriteDescriptorSet write{
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = element,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = &info,
    };
    g_device->device.updateDescriptorSets(write, {}, g_device->dispatch);
}

void ComputeBarrier(vk::CommandBuffer cmd, vk::PipelineStageFlags2 src_stage,
                    vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask =
            vk::AccessFlagBits2::eShaderSampledRead | vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &barrier},
                         g_device->dispatch);
}

bool CreateVolumes() {
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage |
                                      vk::ImageUsageFlagBits::eSampled |
                                      vk::ImageUsageFlagBits::eTransferDst;
    for (auto* volumes : {&g_density, &g_darkness, &g_shadow}) {
        for (Image& image : *volumes) {
            image = g_memory->CreateVolume(CellFormat, g_cells_extent, usage);
        }
    }
    g_scattered = g_memory->CreateVolume(ScatterFormat, g_cells_extent, usage);
    for (u32 i = 0; i < Layers; i++) {
        g_maps[i] = g_memory->CreateVolume(CellFormat, MapExtent, usage);
        g_map_darkness[i] = g_memory->CreateVolume(CellFormat, MapExtent, usage);
    }
    for (Image& image : g_noise) {
        image = g_memory->CreateVolume(CellFormat, NoiseExtent,
                                       vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eTransferDst);
    }
    g_staging = g_memory->CreateBuffer(Noises * NoiseBytes, vk::BufferUsageFlagBits::eTransferSrc,
                                       Placement::Staging);
    for (const auto* volumes : {&g_density, &g_darkness, &g_shadow}) {
        for (const Image& image : *volumes) {
            if (!image.image) {
                return false;
            }
        }
    }
    for (u32 i = 0; i < Layers; i++) {
        if (!g_maps[i].image || !g_map_darkness[i].image || !g_noise[i].image) {
            return false;
        }
    }
    return g_scattered.image && g_staging.mapped != nullptr;
}

bool CreateSets() {
    const auto& d = *g_device;
    constexpr auto Compute = vk::ShaderStageFlagBits::eCompute;
    const std::array<vk::DescriptorSetLayoutBinding, 2> bake{{
        {0, Combined, 1, Compute},
        {1, Storage, 1, Compute},
    }};
    // The last frame's density and darkness, each map and its darkness, the frame's density and
    // darkness.
    const std::array<vk::DescriptorSetLayoutBinding, 8> cells{{
        {0, Combined, 1, Compute},
        {1, Combined, 1, Compute},
        {2, Combined, 1, Compute},
        {3, Combined, 1, Compute},
        {4, Combined, 1, Compute},
        {5, Combined, 1, Compute},
        {6, Storage, 1, Compute},
        {7, Storage, 1, Compute},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 5> scatter{{
        {0, Combined, 1, Compute},
        {1, Combined, 1, Compute},
        {2, Combined, 1, Compute},
        {3, Combined, 1, Compute},
        {4, Storage, 1, Compute},
    }};
    const std::array<vk::DescriptorSetLayoutBinding, 2> apply{{
        {0, Combined, 1, vk::ShaderStageFlagBits::eFragment},
        {1, Combined, 1, vk::ShaderStageFlagBits::eFragment},
    }};
    g_bake_layout = CreateLayout(bake);
    g_darkness_layout = CreateLayout(bake);
    g_cell_layout = CreateLayout(cells);
    g_shadow_layout = CreateLayout(bake);
    g_scatter_layout = CreateLayout(scatter);
    g_apply_layout = CreateLayout(apply);
    // Bakes 2, darkness 2, cells 2, shadows 2, scattering 4, apply 1.
    constexpr u32 Sets = 13;
    const std::array<vk::DescriptorPoolSize, 2> sizes{{
        {Combined, 2 + 2 + 2 * 6 + 2 + 4 * 4 + 2},
        {Storage, 2 + 2 + 2 * 2 + 2 + 4},
    }};
    g_pool = d.device
                 .createDescriptorPool({.maxSets = Sets,
                                        .poolSizeCount = static_cast<u32>(sizes.size()),
                                        .pPoolSizes = sizes.data()},
                                       nullptr, d.dispatch)
                 .value;
    if (!g_bake_layout || !g_darkness_layout || !g_cell_layout || !g_shadow_layout ||
        !g_scatter_layout || !g_apply_layout || !g_pool) {
        return false;
    }
    const std::array<vk::DescriptorSetLayout, Sets> layouts{
        g_bake_layout,    g_bake_layout,    g_darkness_layout, g_darkness_layout, g_cell_layout,
        g_cell_layout,    g_shadow_layout,  g_shadow_layout,   g_scatter_layout,  g_scatter_layout,
        g_scatter_layout, g_scatter_layout, g_apply_layout,
    };
    const auto [result, sets] = d.device.allocateDescriptorSets(
        {.descriptorPool = g_pool, .descriptorSetCount = Sets, .pSetLayouts = layouts.data()},
        d.dispatch);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    for (u32 i = 0; i < 2; i++) {
        g_bake_sets[i] = sets[i];
        g_darkness_sets[i] = sets[2 + i];
        g_cell_sets[i] = sets[4 + i];
        g_shadow_sets[i] = sets[6 + i];
        g_scatter_sets[i][0] = sets[8 + 2 * i];
        g_scatter_sets[i][1] = sets[9 + 2 * i];
    }
    g_apply_set = sets[12];
    for (u32 i = 0; i < Layers; i++) {
        Write(g_bake_sets[i], 0, 0, Combined, g_noise[FbmNoise], g_wrap_sampler);
        Write(g_bake_sets[i], 1, 0, Storage, g_maps[i], {});
        Write(g_darkness_sets[i], 0, 0, Combined, g_maps[i], g_map_sampler);
        Write(g_darkness_sets[i], 1, 0, Storage, g_map_darkness[i], {});
    }
    for (u32 i = 0; i < 2; i++) {
        const u32 last = 1 - i;
        Write(g_cell_sets[i], 0, 0, Combined, g_density[last], g_clamp_sampler);
        Write(g_cell_sets[i], 1, 0, Combined, g_darkness[last], g_clamp_sampler);
        for (u32 k = 0; k < Layers; k++) {
            Write(g_cell_sets[i], 2 + 2 * k, 0, Combined, g_maps[k], g_map_sampler);
            Write(g_cell_sets[i], 3 + 2 * k, 0, Combined, g_map_darkness[k], g_map_sampler);
        }
        Write(g_cell_sets[i], 6, 0, Storage, g_density[i], {});
        Write(g_cell_sets[i], 7, 0, Storage, g_darkness[i], {});
        Write(g_shadow_sets[i], 0, 0, Combined, g_shadow[last], g_clamp_sampler);
        Write(g_shadow_sets[i], 1, 0, Storage, g_shadow[i], {});
        for (u32 s = 0; s < 2; s++) {
            const vk::DescriptorSet set = g_scatter_sets[i][s];
            Write(set, 0, 0, Combined, g_noise[PerlinNoise], g_wrap_sampler);
            Write(set, 1, 0, Combined, g_density[i], g_nearest_sampler);
            Write(set, 2, 0, Combined, g_darkness[i], g_nearest_sampler);
            Write(set, 3, 0, Combined, g_shadow[s], g_nearest_sampler);
            Write(set, 4, 0, Storage, g_scattered, {});
        }
    }
    Write(g_apply_set, 1, 0, Combined, g_scattered, g_clamp_sampler);
    return true;
}

// Moves every volume to the general layout and clears the histories: no cloud, no darkness, all
// sunlit, nothing scattered and everything shown.
void Clear(vk::CommandBuffer cmd) {
    const auto& dl = g_device->dispatch;
    std::vector<const Image*> volumes{&g_scattered};
    for (const auto* set : {&g_density, &g_darkness, &g_shadow, &g_maps, &g_map_darkness}) {
        for (const Image& image : *set) {
            volumes.push_back(&image);
        }
    }
    for (const Image& image : g_noise) {
        volumes.push_back(&image);
    }
    std::vector<vk::ImageMemoryBarrier2> barriers;
    for (const Image* image : volumes) {
        // The noise is copied, the rest cleared.
        barriers.push_back({
            .dstStageMask = vk::PipelineStageFlagBits2::eClear | vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = image->image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
    }
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = static_cast<u32>(barriers.size()),
                          .pImageMemoryBarriers = barriers.data()},
                         dl);
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const auto clear = [&](const Image& image, float value, float alpha) {
        cmd.clearColorImage(image.image, vk::ImageLayout::eGeneral,
                            vk::ClearColorValue{.float32 = std::array{value, value, value, alpha}},
                            range, dl);
    };
    for (u32 i = 0; i < 2; i++) {
        clear(g_density[i], 0.0f, 0.0f);
        clear(g_darkness[i], 0.0f, 0.0f);
        clear(g_shadow[i], 1.0f, 1.0f);
    }
    clear(g_scattered, 0.0f, 1.0f);
    ComputeBarrier(cmd, vk::PipelineStageFlagBits2::eClear, vk::AccessFlagBits2::eTransferWrite,
                   vk::PipelineStageFlagBits2::eComputeShader |
                       vk::PipelineStageFlagBits2::eFragmentShader);
}

// Copies the game's noise volumes once they exist; their bytes stay as the constructor made them.
void CopyNoise(vk::CommandBuffer cmd, const Scene::Header& header) {
    bool copied = false;
    for (u32 i = 0; i < Noises; i++) {
        const u64 source = header.fog_noise[i];
        if (source == 0 || source == g_noise_source[i]) {
            continue;
        }
        std::memcpy(g_staging.mapped + i * NoiseBytes, reinterpret_cast<const void*>(source),
                    NoiseBytes);
        const vk::BufferImageCopy region{
            .bufferOffset = i * NoiseBytes,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = NoiseExtent,
        };
        cmd.copyBufferToImage(g_staging.buffer, g_noise[i].image, vk::ImageLayout::eGeneral, region,
                              g_device->dispatch);
        g_noise_source[i] = source;
        copied = true;
    }
    if (copied) {
        g_maps_stale = true;
        ComputeBarrier(cmd, vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
                       vk::PipelineStageFlagBits2::eComputeShader);
    }
}

bool Ready() {
    return g_enabled && g_noise_source[PerlinNoise] != 0 && g_noise_source[FbmNoise] != 0;
}

u32 Groups(u32 cells, u32 size) {
    return (cells + size - 1) / size;
}

// Makes the cloud maps and their darkness again where the header counts changes.
void Bake(vk::CommandBuffer cmd, const Scene::Header& header, u64 fog) {
    const auto& dl = g_device->dispatch;
    const auto compute = vk::PipelineBindPoint::eCompute;
    bool baked = false;
    for (u32 i = 0; i < Layers; i++) {
        const bool maps = g_maps_stale || header.fog_maps[i] != g_maps_made[i];
        if (!maps && header.fog_darkness[i] == g_darkness_made[i]) {
            continue;
        }
        if (maps) {
            const BakePush push{.fog = fog, .layer = i};
            const std::array sets{g_bake_sets[i], Textures::Set()};
            cmd.bindPipeline(compute, g_bake.pipeline, dl);
            cmd.bindDescriptorSets(compute, g_bake.layout, 0, sets, {}, dl);
            cmd.pushConstants(g_bake.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                              &push, dl);
            cmd.dispatch(Groups(MapExtent.width, GroupSize), Groups(MapExtent.height, GroupSize),
                         MapExtent.depth, dl);
            ComputeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                           vk::AccessFlagBits2::eShaderStorageWrite,
                           vk::PipelineStageFlagBits2::eComputeShader);
        }
        const FogPush push{.fog = fog};
        cmd.bindPipeline(compute, g_darkness_pipeline.pipeline, dl);
        cmd.bindDescriptorSets(compute, g_darkness_pipeline.layout, 0, g_darkness_sets[i], {}, dl);
        cmd.pushConstants(g_darkness_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0,
                          sizeof(push), &push, dl);
        cmd.dispatch(Groups(MapExtent.width, GroupSize), Groups(MapExtent.height, GroupSize),
                     MapExtent.depth, dl);
        g_maps_made[i] = header.fog_maps[i];
        g_darkness_made[i] = header.fog_darkness[i];
        baked = true;
    }
    g_maps_stale = false;
    if (baked) {
        ComputeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                       vk::AccessFlagBits2::eShaderStorageWrite,
                       vk::PipelineStageFlagBits2::eComputeShader);
    }
}

} // namespace

vk::ImageView ScatteredView() {
    return g_scattered.view;
}

bool Start(const Device& device, Memory& memory, vk::Format scene_format) {
    g_device = &device;
    g_memory = &memory;
    g_cells_extent = CellExtents[std::min(static_cast<u32>(Settings::Get().graphics.fog), 2u)];
    if (!device.storage_extended_formats) {
        LOG_WARNING(Render_Vulkan, "GR2port: no 8-bit storage images, so no fog");
        return true;
    }
    g_clamp_sampler = CreateSampler(vk::Filter::eLinear, vk::SamplerAddressMode::eClampToEdge,
                                    vk::SamplerAddressMode::eClampToEdge);
    g_wrap_sampler = CreateSampler(vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat,
                                   vk::SamplerAddressMode::eRepeat);
    g_map_sampler = CreateSampler(vk::Filter::eLinear, vk::SamplerAddressMode::eRepeat,
                                  vk::SamplerAddressMode::eClampToBorder);
    g_nearest_sampler = CreateSampler(vk::Filter::eNearest, vk::SamplerAddressMode::eClampToEdge,
                                      vk::SamplerAddressMode::eClampToEdge);
    if (!g_clamp_sampler || !g_wrap_sampler || !g_map_sampler || !g_nearest_sampler ||
        !CreateVolumes() || !CreateSets()) {
        LOG_ERROR(Render_Vulkan, "GR2port: the fog's volumes or sets failed");
        return false;
    }
    const std::array bake_sets{g_bake_layout, Textures::SetLayout()};
    const std::array darkness_sets{g_darkness_layout};
    const std::array cell_sets{g_cell_layout, Textures::SetLayout()};
    const std::array shadow_sets{g_shadow_layout, Shadows::SetLayout()};
    const std::array scatter_sets{g_scatter_layout};
    const std::array apply_sets{g_apply_layout};
    g_bake = CreateComputePipeline(device, FOG_BAKE_COMP, sizeof(BakePush), bake_sets);
    g_darkness_pipeline =
        CreateComputePipeline(device, FOG_DARKNESS_COMP, sizeof(FogPush), darkness_sets);
    // The cell programs' grid, constant ids 0 to 2 of fog.glsl.
    const std::array<s32, 3> grid{static_cast<s32>(g_cells_extent.width),
                                  static_cast<s32>(g_cells_extent.height),
                                  static_cast<s32>(g_cells_extent.depth)};
    const std::array<vk::SpecializationMapEntry, 3> grid_entries{{
        {0, 0, sizeof(s32)},
        {1, sizeof(s32), sizeof(s32)},
        {2, 2 * sizeof(s32), sizeof(s32)},
    }};
    const vk::SpecializationInfo grid_info{
        .mapEntryCount = static_cast<u32>(grid_entries.size()),
        .pMapEntries = grid_entries.data(),
        .dataSize = sizeof(grid),
        .pData = grid.data(),
    };
    g_cells_pipeline =
        CreateComputePipeline(device, FOG_DENSITY_COMP, sizeof(CellPush), cell_sets, &grid_info);
    g_shadow_pipeline =
        CreateComputePipeline(device, FOG_SHADOW_COMP, sizeof(CellPush), shadow_sets, &grid_info);
    g_scatter =
        CreateComputePipeline(device, FOG_SCATTER_COMP, sizeof(FogPush), scatter_sets, &grid_info);
    g_apply = CreateGraphicsPipeline(device, {
                                                 .vertex = FULLSCREEN_VERT,
                                                 .fragment = FOG_APPLY_FRAG,
                                                 .color = scene_format,
                                                 .premultiplied_blend = true,
                                                 .push_constant_bytes = sizeof(FogPush),
                                                 .set_layouts = apply_sets,
                                             });
    g_enabled = g_bake.pipeline && g_darkness_pipeline.pipeline && g_cells_pipeline.pipeline &&
                g_shadow_pipeline.pipeline && g_scatter.pipeline && g_apply.pipeline;
    return g_enabled;
}

void Resize(vk::ImageView depth) {
    if (!g_enabled) {
        return;
    }
    const Image depth_image{.view = depth};
    Write(g_apply_set, 0, 0, Combined, depth_image, g_nearest_sampler,
          vk::ImageLayout::eDepthStencilReadOnlyOptimal);
}

void RecordVolumes(vk::CommandBuffer cmd, const Scene::Header& header, u64 address) {
    if (!g_enabled) {
        return;
    }
    const auto& dl = g_device->dispatch;
    const auto compute = vk::PipelineBindPoint::eCompute;
    if (!g_cleared) {
        g_cleared = true;
        Clear(cmd);
    }
    // The last frame's passes read what this frame's write.
    ComputeBarrier(cmd,
                   vk::PipelineStageFlagBits2::eComputeShader |
                       vk::PipelineStageFlagBits2::eFragmentShader,
                   vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eComputeShader);
    CopyNoise(cmd, header);
    if (!Ready()) {
        return;
    }
    const u64 fog = address + Scene::FogOffset;
    Bake(cmd, header, fog);
    PassTimes::Mark(cmd, PassTimes::Pass::FogBake);

    g_cells ^= 1;
    const CellPush cells{.fog = fog, .light = address + Scene::LightOffset};
    const std::array cell_sets{g_cell_sets[g_cells], Textures::Set()};
    cmd.bindPipeline(compute, g_cells_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(compute, g_cells_pipeline.layout, 0, cell_sets, {}, dl);
    cmd.pushConstants(g_cells_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(cells),
                      &cells, dl);
    cmd.dispatch(Groups(g_cells_extent.width, GroupSize), Groups(g_cells_extent.height, GroupSize),
                 g_cells_extent.depth, dl);
    PassTimes::Mark(cmd, PassTimes::Pass::FogDensity);
    if ((header.fog & Scene::FogShadow) != 0) {
        g_shadows ^= 1;
        const std::array shadow_sets{g_shadow_sets[g_shadows], Shadows::Set()};
        cmd.bindPipeline(compute, g_shadow_pipeline.pipeline, dl);
        cmd.bindDescriptorSets(compute, g_shadow_pipeline.layout, 0, shadow_sets, {}, dl);
        cmd.pushConstants(g_shadow_pipeline.layout, vk::ShaderStageFlagBits::eCompute, 0,
                          sizeof(cells), &cells, dl);
        cmd.dispatch(Groups(g_cells_extent.width, GroupSize),
                     Groups(g_cells_extent.height, GroupSize), g_cells_extent.depth, dl);
        PassTimes::Mark(cmd, PassTimes::Pass::FogShadow);
    }
    ComputeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderStorageWrite,
                   vk::PipelineStageFlagBits2::eComputeShader);

    const FogPush push{.fog = fog};
    cmd.bindPipeline(compute, g_scatter.pipeline, dl);
    cmd.bindDescriptorSets(compute, g_scatter.layout, 0, g_scatter_sets[g_cells][g_shadows], {},
                           dl);
    cmd.pushConstants(g_scatter.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push,
                      dl);
    cmd.dispatch(Groups(g_cells_extent.width, ScatterGroupSize), g_cells_extent.height, 1, dl);
    ComputeBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
                   vk::AccessFlagBits2::eShaderStorageWrite,
                   vk::PipelineStageFlagBits2::eFragmentShader);
}

void RecordApply(vk::CommandBuffer cmd, u64 address, vk::Extent2D extent) {
    if (!Ready()) {
        return;
    }
    const auto& dl = g_device->dispatch;
    const vk::Viewport viewport{
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .maxDepth = 1.0f,
    };
    const FogPush push{.fog = address + Scene::FogOffset};
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_apply.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_apply.layout, 0, g_apply_set, {},
                           dl);
    cmd.setViewport(0, viewport, dl);
    cmd.setScissor(0, vk::Rect2D{{0, 0}, extent}, dl);
    cmd.pushConstants(g_apply.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push), &push,
                      dl);
    cmd.draw(3, 1, 0, 0, dl);
}

void Log(const u8* packet) {
    Scene::Header header;
    std::memcpy(&header, packet, sizeof(header));
    Scene::Fog fog;
    std::memcpy(&fog, packet + Scene::FogOffset, sizeof(fog));
    const float depth_start = -fog.projection[3];
    LOG_INFO(Render_Vulkan,
             "GR2port fog: on {}, ready {}, noise {:#x} {:#x}, sun shadow {}, map changes {} {} "
             "(made {} {}), darkness changes {} {} (made {} {}), cumulus {} and for the maps {} "
             "{}; depth {:.2f} to {:.1f}; sun ({:.3f} {:.3f} {:.3f}), least extinction {:.5f}, "
             "phases {:.3f} {:.3f}; clouds ({:.3f} {:.3f} {:.3f}); fade to ({:.3f} {:.3f} {:.3f}) "
             "by {:.3f}; height fog top {:.2f} density {:.4f}; depth scale {:.3f}, darkness "
             "scale {:.3f}, noise frequency {:.4f} scale {:.3f} attenuation {:.4f}, shadow {:.1f} "
             "on clouds {:.2f}; cumulus densities {:.4f} {:.4f} {:.4f}, map densities {:.4f} "
             "{:.4f}",
             g_enabled, Ready(), header.fog_noise[PerlinNoise], header.fog_noise[FbmNoise],
             (header.fog & Scene::FogShadow) != 0, header.fog_maps[0], header.fog_maps[1],
             g_maps_made[0], g_maps_made[1], header.fog_darkness[0], header.fog_darkness[1],
             g_darkness_made[0], g_darkness_made[1], fog.cumulus_texture, fog.bake_cumulus[0],
             fog.bake_cumulus[1], depth_start, depth_start - fog.projection[2], fog.light_color[0],
             fog.light_color[1], fog.light_color[2], fog.light_color[3], 0.5f * fog.phase_air[2],
             0.5f * fog.phase_cloud[2], fog.cloud_color[0], fog.cloud_color[1], fog.cloud_color[2],
             fog.apply_color[0], fog.apply_color[1], fog.apply_color[2], fog.apply_color[3],
             fog.puddle_height, fog.puddle_density, fog.depth_scale, fog.darkness_scale,
             fog.noise[3], fog.noise_scale, fog.noise_attenuation, fog.shadow_darkness,
             fog.cloud_shadow, fog.cumulus[0][2], fog.cumulus[1][2], fog.cumulus[2][2],
             fog.layers[0][2], fog.layers[1][2]);
}

} // namespace Gr2Port::Fog
