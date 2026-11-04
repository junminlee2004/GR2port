// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>

#include <xbyak/xbyak.h>

#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "core/libraries/error_codes.h"
#include "core/memory.h"
#include "video_core/gr2_pt/caster_batch.h"
#include "video_core/gr2_pt/caster_renderer.h"
#include "video_core/gr2_pt/gr2_pt.h"
#include "video_core/gr2_pt/marker.h"
#include "video_core/gr2_pt/pass_timing.h"
#include "video_core/gr2_pt/shadow_pass.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

namespace Gr2Pt {

namespace {

constexpr u64 StubAreaSize = 16_KB;
// Past the module's 8 MB trampoline area and within reach of a rel32 jump from the executable.
constexpr u64 StubAreaOffset = 32_MB;

} // namespace

void Install(VAddr module_base) {
    if (module_base != MemoryPatcher::g_eboot_address || !Common::IsGravityRush2()) {
        return;
    }
    void* area =
        reinterpret_cast<void*>(module_base + MemoryPatcher::g_eboot_image_size + StubAreaOffset);
    if (Core::Memory::Instance()->MapMemory(
            &area, reinterpret_cast<VAddr>(area), StubAreaSize,
            Core::MemoryProt::CpuReadWrite | Core::MemoryProt::CpuExec,
            Core::MemoryMapFlags::NoFlags, Core::VMAType::Code, "GR2 PT") != ORBIS_OK ||
        reinterpret_cast<VAddr>(area) + StubAreaSize - module_base >
            std::numeric_limits<s32>::max()) {
        LOG_ERROR(Render_Vulkan, "GR2 PT: no memory for the hook stubs near the executable");
        return;
    }
    Xbyak::CodeGenerator c(StubAreaSize, area);
    if (ShadowPass::Install(module_base, c, StubAreaSize) &&
        CasterBatch::Install(module_base, c, StubAreaSize)) {
        LOG_INFO(Render_Vulkan, "GR2 PT: shadow caster passes are batched ({} stub bytes)",
                 c.getSize());
    }
}

void OnMarker(Vulkan::Rasterizer& rasterizer, const u32* data, u32 dwords) {
    Marker::Words words;
    if (dwords < 1 + Marker::WordCount) {
        return;
    }
    std::memcpy(&words, data + 1, sizeof(words));
    switch (words.kind) {
    case Marker::Kind::PassBegin:
        CasterRenderer::Prepare(rasterizer);
        PassTiming::OnMarker(rasterizer, words);
        break;
    case Marker::Kind::PassEnd:
        PassTiming::OnMarker(rasterizer, words);
        break;
    case Marker::Kind::CasterBatch:
        CasterRenderer::OnBatch(rasterizer, words);
        break;
    case Marker::Kind::CasterSnapshot:
        CasterRenderer::OnSnapshot(rasterizer, words);
        break;
    }
}

const Vulkan::Instance& RasterizerAccess::Instance(const Vulkan::Rasterizer& rasterizer) {
    return rasterizer.instance;
}

const AmdGpu::Liverpool& RasterizerAccess::Gpu(const Vulkan::Rasterizer& rasterizer) {
    return *rasterizer.liverpool;
}

VideoCore::BufferCache& RasterizerAccess::Buffers(Vulkan::Rasterizer& rasterizer) {
    return rasterizer.buffer_cache;
}

VideoCore::TextureCache& RasterizerAccess::Textures(Vulkan::Rasterizer& rasterizer) {
    return rasterizer.texture_cache;
}

Vulkan::Runtime& RasterizerAccess::Barriers(Vulkan::Rasterizer& rasterizer) {
    return rasterizer.runtime;
}

void RasterizerAccess::UpdatePassState(const Vulkan::Rasterizer& rasterizer) {
    rasterizer.UpdateViewportScissorState();
    rasterizer.UpdateDepthStencilState();
    rasterizer.UpdatePrimitiveState(true);
    rasterizer.UpdateRasterizationState();
}

} // namespace Gr2Pt
