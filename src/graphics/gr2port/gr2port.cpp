// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <limits>

#include <xbyak/xbyak.h>

#include "common/gravity_rush.h"
#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "core/game_check/arrival_hang.h"
#include "core/game_check/game_patches.h"
#include "core/libraries/error_codes.h"
#include "core/memory.h"
#include "graphics/gr2port/camera_reach.h"
#include "graphics/gr2port/capture.h"
#include "graphics/gr2port/catcher_outside.h"
#include "graphics/gr2port/catcher_stacks.h"
#include "graphics/gr2port/comic_camera.h"
#include "graphics/gr2port/cord_bend.h"
#include "graphics/gr2port/exit_rows.h"
#include "graphics/gr2port/field_of_view.h"
#include "graphics/gr2port/float_spin.h"
#include "graphics/gr2port/frame_graph_draws.h"
#include "graphics/gr2port/frame_windows.h"
#include "graphics/gr2port/gpu_writes.h"
#include "graphics/gr2port/gr2port.h"
#include "graphics/gr2port/graphics_menu.h"
#include "graphics/gr2port/gravity_kick.h"
#include "graphics/gr2port/gravity_slide.h"
#include "graphics/gr2port/hair_flutter.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/hud_parts.h"
#include "graphics/gr2port/layout_clock.h"
#include "graphics/gr2port/loading_tip.h"
#include "graphics/gr2port/marker_fit.h"
#include "graphics/gr2port/meshes.h"
#include "graphics/gr2port/mob_float.h"
#include "graphics/gr2port/motion_ends.h"
#include "graphics/gr2port/mouse_camera.h"
#include "graphics/gr2port/movies.h"
#include "graphics/gr2port/part_hits.h"
#include "graphics/gr2port/particle_capture.h"
#include "graphics/gr2port/photo_ui.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/player_control.h"
#include "graphics/gr2port/renderer.h"
#include "graphics/gr2port/screen_shake.h"
#include "graphics/gr2port/screenshot.h"
#include "graphics/gr2port/script_timers.h"
#include "graphics/gr2port/scripted_moves.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/shadow_distance.h"
#include "graphics/gr2port/smoke_and_dust.h"
#include "graphics/gr2port/sound_delay.h"
#include "graphics/gr2port/special_cameras.h"
#include "graphics/gr2port/stick_tilt.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/timeline.h"
#include "graphics/gr2port/ui_capture.h"
#include "graphics/gr2port/ui_items.h"
#include "graphics/gr2port/water_splashes.h"

namespace Gr2Port {

namespace {

constexpr u64 StubAreaSize = 16_KB;
// Past the module's 8 MB trampoline area and within reach of a rel32 jump from the executable.
constexpr u64 StubAreaOffset = 32_MB;

// The frame graph FUN_0142ec00: the game's draw thread calls it once per frame.
constexpr Hooks::Entry FrameGraph{0x142ec00, 6, Hooks::FramePrologue};

// The draw thread's change to its next context, at the end of each frame.
constexpr Hooks::Entry NextContext{0x16147e0, 6, Hooks::FramePrologue};

// The particle manager's first and last steps of a frame, inside the frame graph.
constexpr Hooks::Entry ParticleSetup{0x14a31c0, 6, Hooks::FramePrologue};
constexpr Hooks::Entry ParticleList{0x14a3ba0, 6, Hooks::FramePrologue};

void PS4_SYSV_ABI OnParticleSetup(u32, const Hooks::Registers* registers, u64) {
    Capture::OnParticleSetup(registers->rdi);
}

void PS4_SYSV_ABI OnParticleList(u32, const Hooks::Arguments* arguments, u64) {
    Capture::OnParticleList(arguments->rdi);
}

// The engine's manager fills the simulation's tables in FUN_00766b70, an effect at a time in
// FUN_00763370(manager, effect, record, object, owners): mov rax, [rcx]; test rax, rax.
constexpr Hooks::Entry ParticleTables{0x766b70, 6, Hooks::FramePrologue};
constexpr Hooks::Entry ParticleEffect{0x763370, 6, {0x48, 0x8b, 0x01, 0x48, 0x85, 0xc0}};

void PS4_SYSV_ABI OnParticleEffect(u32, const Hooks::Arguments* arguments, u64) {
    ParticleCapture::OnEffectFilled(static_cast<u16>(arguments->rsi), arguments->rdx,
                                    arguments->r8);
}

void PS4_SYSV_ABI OnParticleTables(u32, const Hooks::Arguments* arguments, u64) {
    ParticleCapture::OnTablesFilled(arguments->rdi);
}

// Before it fills them, FUN_00766b70 zeros the owners and the effects of the ring's slot with
// memset: call memset at 0x766bd7 and at 0x766c20. FUN_00771390 frees the memory of a buffer, the
// tables' among others.
constexpr Hooks::Entry ClearOwners{0x766bd7, 5, {0xe8, 0xd4, 0x17, 0x09, 0x01}};
constexpr VAddr ClearOwnersNext = 0x766bdc;
constexpr Hooks::Entry ClearEffects{0x766c20, 5, {0xe8, 0x8b, 0x17, 0x09, 0x01}};
constexpr VAddr ClearEffectsNext = 0x766c25;
constexpr Hooks::Entry FreeBuffer{0x771390, 6, Hooks::FramePrologueR14};

void PS4_SYSV_ABI OnFreeBuffer(u32, const Hooks::Registers*, u64) {
    ParticleCapture::OnBufferFreed();
}

// FUN_0142ec00(T + 0xa0, view, ...): rdi is the frame graph, rsi the view the frame draws.
void PS4_SYSV_ABI OnFrameGraph(u32, const Hooks::Registers* registers, u64) {
    Capture::OnFrame(registers->rdi, registers->rsi);
    GameCheck::ArrivalHang::OnWorldFrame();
    ScriptTimers::OnFrame();
}

void PS4_SYSV_ABI OnFrameGraphEnd(u32, const Hooks::Arguments*, u64) {
    UiCapture::OnFrameGraphEnd();
}

void PS4_SYSV_ABI OnNextContext(u32, const Hooks::Registers*, u64) {
    Capture::OnFrameEnd();
}

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
            Core::MemoryMapFlags::NoFlags, Core::VMAType::Code, "GR2port") != ORBIS_OK ||
        reinterpret_cast<VAddr>(area) + StubAreaSize - module_base >
            std::numeric_limits<s32>::max()) {
        LOG_ERROR(Render_Vulkan, "GR2port: no memory for the hook stubs near the executable");
        return;
    }
    Xbyak::CodeGenerator c(StubAreaSize, area);
    // Without these the draw thread waits forever on the frame counters of its contexts.
    if (!GpuWrites::Install(module_base, c, StubAreaSize)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the game's GPU writers are not the expected code");
    }
    const Hooks::Result frame =
        Hooks::ObserveEntry(module_base, FrameGraph, c, StubAreaSize, &OnFrameGraph, 0);
    if (!Hooks::Hooked(frame)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the frame graph's entry is not the expected code");
        return;
    }
    UiCapture::Install(module_base);
    UiItems::Install(module_base, c, StubAreaSize);
    PhotoUi::RouteItems(module_base, c, StubAreaSize);
    PhotoUi::Install(module_base);
    PhotoUi::WatchCamera(module_base, c, StubAreaSize);
    HudParts::Install(module_base);
    MouseCamera::Install(module_base, c, StubAreaSize);
    StickTilt::Install(module_base, c, StubAreaSize);
    CameraReach::Install(module_base, c, StubAreaSize);
    Timeline::Install(module_base, c, StubAreaSize);
    PlayerControl::Install(module_base, c, StubAreaSize);
    if (ExitRows::Install(module_base, c, StubAreaSize)) {
        GraphicsMenu::Install(module_base, c, StubAreaSize);
        PhotoUi::AddRow(module_base, c, StubAreaSize);
        LoadingTip::Install(module_base, c, StubAreaSize);
        Screenshot::Install();
    }
    MobFloat::Install(module_base, c, StubAreaSize);
    HairFlutter::Install(module_base, c, StubAreaSize);
    GravityKick::Install(module_base, c, StubAreaSize);
    GravitySlide::Install(module_base, c, StubAreaSize);
    ScriptTimers::Install(module_base, c, StubAreaSize);
    Catcher::Stacks::SetGame(module_base);
    Catcher::Outside::Set(Catcher::Outside::Crumb::GameBase, module_base);
    WaterSplashes::Install(module_base, c, StubAreaSize);
    SmokeAndDust::Install(module_base, c, StubAreaSize);
    ShadowDistance::Install(module_base, c, StubAreaSize);
    FloatSpin::Install(module_base, c, StubAreaSize);
    // At the game's own 30 fps the motions, the hits and the comics keep the stock code.
    if (Settings::Get().game.frame_rate > 30) {
        MotionEnds::Install(module_base, c, StubAreaSize);
        PartHits::Install(module_base, c, StubAreaSize);
        ComicCamera::Install(module_base, c, StubAreaSize);
        LayoutClock::Install(module_base, c, StubAreaSize);
        ScriptedMoves::Install(module_base, c, StubAreaSize);
        CordBend::Install(module_base, c, StubAreaSize);
        FrameWindows::Install(module_base, c, StubAreaSize);
        ScriptTimers::InstallFrameCounts(module_base, c, StubAreaSize);
        SoundDelay::Install(module_base, c, StubAreaSize);
    }
    FieldOfView::Install(module_base, c, StubAreaSize);
    ScreenShake::Install(module_base, c, StubAreaSize);
    SpecialCameras::Install(module_base, c, StubAreaSize);
    if (GamePatches::FitsUi()) {
        MarkerFit::Install(module_base, c, StubAreaSize);
    }
    if (Hooks::ObserveReturn(module_base, FrameGraph, c, StubAreaSize, &OnFrameGraphEnd, 0) ==
            Hooks::Result::Mismatch ||
        Hooks::ObserveEntry(module_base, NextContext, c, StubAreaSize, &OnNextContext, 0) ==
            Hooks::Result::Mismatch) {
        LOG_ERROR(Render_Vulkan, "GR2port: the screen drawing's hooks are not the expected code");
    }
    if (Hooks::ObserveEntry(module_base, ParticleSetup, c, StubAreaSize, &OnParticleSetup, 0) ==
            Hooks::Result::Mismatch ||
        Hooks::ObserveReturn(module_base, ParticleList, c, StubAreaSize, &OnParticleList, 0) ==
            Hooks::Result::Mismatch) {
        LOG_ERROR(Render_Vulkan, "GR2port: the particle manager is not the expected code");
    }
    // Without these the capture reads the whole of the simulation's tables each frame.
    const Hooks::Result effect_filled =
        Hooks::ObserveReturn(module_base, ParticleEffect, c, StubAreaSize, &OnParticleEffect, 0);
    const Hooks::Result tables_filled =
        Hooks::ObserveReturn(module_base, ParticleTables, c, StubAreaSize, &OnParticleTables, 0);
    if (effect_filled == Hooks::Result::Mismatch || tables_filled == Hooks::Result::Mismatch) {
        LOG_ERROR(Render_Vulkan, "GR2port: the particle tables' fill is not the expected code");
    } else if (Hooks::Hooked(effect_filled) && Hooks::Hooked(tables_filled) &&
               Hooks::Hooked(Hooks::ObserveEntry(module_base, FreeBuffer, c, StubAreaSize,
                                                 &OnFreeBuffer, 0))) {
        // The manager zeros the lines a table's last fill wrote, not the whole table.
        using namespace Xbyak::util;
        const auto clear = [&](VAddr next) {
            const void* stub = c.getCurr();
            c.mov(rax, reinterpret_cast<u64>(&ParticleCapture::ClearTable));
            c.call(rax);
            c.jmp(Hooks::Code(module_base, next));
            return stub;
        };
        if (Hooks::Replace(module_base, ClearOwners, c, StubAreaSize, clear(ClearOwnersNext)) !=
                Hooks::Result::Hooked ||
            Hooks::Replace(module_base, ClearEffects, c, StubAreaSize, clear(ClearEffectsNext)) !=
                Hooks::Result::Hooked) {
            LOG_ERROR(Render_Vulkan,
                      "GR2port: the particle tables' zeroing is not the expected code");
        }
    }
    if (!Meshes::Install(module_base, c, StubAreaSize) ||
        !Textures::Install(module_base, c, StubAreaSize)) {
        return;
    }
    if (!Movies::Install(module_base, c, StubAreaSize)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the movie update is not the expected code");
    }
    FrameGraphDraws::Install(module_base, c, StubAreaSize);
    Renderer::Start();
    LOG_INFO(Render_Vulkan, "GR2port: renderer started");
}

void WaitForPipelines() {
    const auto start = std::chrono::steady_clock::now();
    if (WaitForPipelineBuilds()) {
        LOG_INFO(Render_Vulkan,
                 "GR2port: the game starts after {} ms of pipeline builds, {} pipelines",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count(),
                 PipelineBuildProgress().made);
    }
}

} // namespace Gr2Port
