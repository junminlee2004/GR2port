// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <span>

#include <fmt/ranges.h>

#include "common/logging/log.h"
#include "video_core/gr2port/capture_math.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/ui_capture.h"

namespace Gr2Port::UiCapture {

namespace {

using Capture::Load;

// The executable's addresses are GoldHEN's, which assume this base.
constexpr VAddr GoldHenBase = 0x400000;
// The draw thread's contexts and the indices of the frame's and the last frame's. A context begins
// with its command stream: its first word, its end and where the next command goes.
constexpr VAddr Contexts = 0x1f315d8;
constexpr u64 ContextBytes = 0xcff8;
constexpr VAddr ContextIndex = 0x1f585f0;
constexpr VAddr PreviousContextIndex = 0x1f585f4;
constexpr u64 StreamBegin = 0x0;
constexpr u64 StreamEnd = 0x8;
constexpr u64 StreamNext = 0x10;
// A context's cached state, which it sends only when it changes: the viewport's x, y, width and
// height, whether colour blends, the blend's two operations and four factors, the primitive type,
// and the index buffer's type and address.
constexpr u64 ContextViewport = 0xcb90;
constexpr u64 ContextBlendOn = 0xcc08;
constexpr u64 ContextBlendOps = 0xcc0c;
constexpr u64 ContextBlendFactors = 0xcc14;
constexpr u64 ContextPrimitive = 0xcc7c;
constexpr u64 ContextIndexType = 0xceb8;
constexpr u64 ContextIndexBase = 0xcec0;
// The frame's back buffer, of three. A target object holds its size at +0x14 and its T# at +0x30.
constexpr VAddr BackBuffers = 0x1f31260;
constexpr u64 BackBufferBytes = 0x118;
constexpr u64 TargetWidth = 0x14;
constexpr u64 TargetHeight = 0x16;
constexpr u64 TargetSharp = 0x30;
// The photo manager, whose target object at +0x580 FUN_011afe90 draws each shot into.
constexpr VAddr PhotoManager = 0x1eed278;
constexpr u64 PhotoObject = 0x580;

// Command packets.
constexpr u32 PacketType2 = 2;
constexpr u32 PacketType3 = 3;
constexpr u32 OpIndexBase = 0x26;
constexpr u32 OpDrawIndex2 = 0x27;
constexpr u32 OpIndexType = 0x2a;
constexpr u32 OpDrawIndexAuto = 0x2d;
constexpr u32 OpDrawIndexOffset2 = 0x35;
constexpr u32 OpSetContextReg = 0x69;
constexpr u32 OpSetShReg = 0x76;
constexpr u32 OpSetUconfigReg = 0x79;

// Registers, as offsets from the first of their kind.
constexpr u32 Registers = 0x400;
constexpr u32 ShPgmLoPs = 0x08;
constexpr u32 ShPgmHiPs = 0x09;
constexpr u32 ShUserDataPs = 0x0c;
constexpr u32 ShPgmLoVs = 0x48;
constexpr u32 ShPgmHiVs = 0x49;
constexpr u32 ShUserDataVs = 0x4c;
// The engine sets the screen scissor to the viewport, and to the target's whole size right after
// it sets a target.
constexpr u32 ContextScreenScissor = 0x0c;
constexpr u32 ContextViewportScaleX = 0x10f;
constexpr u32 ContextViewportOffsetX = 0x110;
constexpr u32 ContextViewportScaleY = 0x111;
constexpr u32 ContextViewportOffsetY = 0x112;
constexpr u32 ContextBlend0 = 0x1e0;
constexpr u32 ContextColor0Base = 0x318;
constexpr u32 UconfigPrimitive = 0x242;

// A vertex program's user data: the fetch program, the table of its inputs' buffers, its constants
// and its palette. A pixel program's: a T#, an S#, and its constants, or with a second texture a
// table of T#, S# and constants.
constexpr u32 VsUserTable = 2;
constexpr u32 VsUserConstants = 4;
constexpr u32 VsUserPalette = 8;
constexpr u32 PsUserSampler = 8;
constexpr u32 PsUserConstants = 12;
constexpr u32 TableSampler = 8;
constexpr u32 TableConstants = 12;

// Where a program's hash lies in its code, after its first instructions, which load two literals.
constexpr u64 ProgramHash = 0xc;

struct VertexProgram {
    u32 hash;
    u32 inputs;
    bool constants;
    bool palette;
};
// In Scene::UiVertexProgram's order.
constexpr std::array<VertexProgram, static_cast<u32>(Scene::UiVertexProgram::Count)> VertexPrograms{
    {
        {0xd77886dc, 3, false, false},
        {0xac069417, 0, false, false},
        {0x5a43fff7, 3, true, false},
        {0x591e6b63, 4, true, false},
        {0x7ff3ec61, 3, true, false},
        {0x239eaa3a, 5, true, true},
        {0x13c2ea85, 6, true, true},
        {0xc1dfc610, 4, true, false},
    }};

struct PixelProgram {
    u32 hash;
    u32 textures;
    bool constants;
};
// In Scene::UiPixelProgram's order.
constexpr std::array<PixelProgram, static_cast<u32>(Scene::UiPixelProgram::Count)> PixelPrograms{{
    {0x0c97af79, 0, false},
    {0xdc63f6b2, 1, false},
    {0x15305830, 0, false},
    {0x7c7ac89f, 1, true},
    {0x3e062520, 1, true},
    {0xbf81268f, 1, true},
    {0x915e83e6, 1, true},
    {0x6b493faa, 2, true},
    {0xb11cba5e, 1, true},
    {0xe2f10454, 2, true},
    {0x69770dd9, 1, true},
    {0xf64d728c, 2, true},
}};

constexpr u32 ReportFrames = 600;
constexpr u32 LoggedUnknowns = 64;
constexpr u32 RecentBuffers = 16;
// TextureHandle's answer for a texture the renderer does not have yet.
constexpr u32 NotReady = ~0u;

VAddr g_base = 0;
VAddr g_context = 0;
VAddr g_start = 0;
std::array<u32, Registers> g_sh{};
std::array<u32, Registers> g_context_registers{};
std::array<u32, Registers> g_uconfig{};
u64 g_index_base = 0;
u32 g_index_bits = 16;
// The size the engine gave the current target, 0 until it sets the scissor after the target.
u32 g_target_width = 0;
u32 g_target_height = 0;

// Kept through the run, as the renderer keeps an image per target.
std::array<Scene::UiTarget, Scene::MaxUiTargets> g_targets{};
u32 g_target_count = 0;

std::array<Scene::UiRun, Scene::MaxUiRuns> g_runs;
std::array<Scene::VertexCommand, Scene::MaxUiDraws> g_commands;
std::array<u32, Scene::MaxUiDynamicTextures> g_dynamic;

u32 g_logged_programs = 0;
u32 g_logged_textures = 0;
std::array<u64, LoggedUnknowns> g_unknown_textures{};
u32 g_frames = 0;
u32 g_world_frames = 0;
u64 g_draws = 0;
u64 g_runs_total = 0;
u64 g_words = 0;
u64 g_bytes = 0;
u64 g_unknown = 0;
u64 g_unported = 0;
u64 g_dropped = 0;
u64 g_write_ns = 0;
u32 g_unread = 0;

struct Buffer {
    u64 base;
    u32 stride;
    u32 elements;
    u32 word3;
};

Buffer ReadBuffer(const u32* words) {
    return {
        .base = u64{words[0]} | u64{words[1] & 0xfff} << 32,
        .stride = words[1] >> 16 & 0x3fff,
        .elements = words[2],
        .word3 = words[3],
    };
}

// The bytes a buffer's elements span: the last element ends at its format's size.
u64 BufferBytes(const Buffer& buffer) {
    u64 element = 4;
    switch (buffer.word3 >> 15 & 15) {
    case 14:
        element = 16;
        break;
    case 13:
        element = 12;
        break;
    case 11:
    case 12:
        element = 8;
        break;
    default:
        break;
    }
    return u64{buffer.stride} * (std::max(buffer.elements, 1u) - 1) + element;
}

u64 ProgramAddress(u32 low, u32 high) {
    return u64{high} << 40 | u64{low} << 8;
}

u64 SharpAddress(const u32* sharp) {
    return (u64{sharp[1] & 0xff} << 32 | sharp[0]) << 8;
}

void Store(std::array<u32, Registers>& registers, const u32* body, u32 values) {
    const u32 first = body[0] & 0xffff;
    if (first < Registers && first + values <= Registers) {
        std::memcpy(&registers[first], body + 1, values * sizeof(u32));
    }
}

u32 FindTarget(u64 base, u32 width, u32 height) {
    for (u32 i = 0; i < g_target_count; i++) {
        if (g_targets[i].base == base) {
            return i;
        }
    }
    if (g_target_count == g_targets.size() || base == 0 || width == 0 || height == 0) {
        return Scene::MaxUiTargets;
    }
    g_targets[g_target_count] = {.base = base, .width = width, .height = height};
    LOG_INFO(Render_Vulkan, "GR2port UI: target {} at {:#x}, {}x{}", g_target_count, base, width,
             height);
    return g_target_count++;
}

// A target object's target.
u32 ObjectTarget(VAddr object) {
    const auto sharp = Load<std::array<u32, 8>>(object + TargetSharp);
    return FindTarget(SharpAddress(sharp.data()), Load<u16>(object + TargetWidth),
                      Load<u16>(object + TargetHeight));
}

// The target of the photo the frame drew, plus one, or 0.
u32 PhotoTarget(const Scene::UiHeader& ui) {
    const VAddr manager = Load<VAddr>(g_base + PhotoManager - GoldHenBase);
    if (manager == 0) {
        return 0;
    }
    const auto sharp = Load<std::array<u32, 8>>(manager + PhotoObject + TargetSharp);
    const u64 base = SharpAddress(sharp.data());
    for (u32 i = 0; i < ui.runs; i++) {
        const u32 target = g_runs[i].target;
        if (target < g_target_count && g_targets[target].base == base) {
            return target + 1;
        }
    }
    return 0;
}

class Writer {
public:
    Writer(u8* packet_, u64 address_, Scene::UiHeader& ui_)
        : packet{packet_}, address{address_}, ui{ui_} {}

    void Draw(u32 opcode, const u32* body);
    void Finish();

    u64 Used() const {
        return used;
    }

private:
    u64 Copy(VAddr from, u64 bytes);
    u64 CopyShared(VAddr from, u64 bytes);
    bool CopyInputs(VAddr table, u32 inputs, Scene::UiDraw& draw);
    u32 TextureHandle(const u32* sharp, bool* linear = nullptr);
    void AddRun(const Scene::UiRun& state);

    u8* packet;
    u64 address;
    Scene::UiHeader& ui;
    u64 used = 0;
    // The buffers copied last, which the next draws often share.
    std::array<VAddr, RecentBuffers> recent_from{};
    std::array<u64, RecentBuffers> recent_bytes{};
    std::array<u64, RecentBuffers> recent_to{};
    u32 next_recent = 0;
};

u64 Writer::Copy(VAddr from, u64 bytes) {
    constexpr u64 Alignment = 16;
    if (from == 0 || bytes == 0 || used + bytes > Scene::MaxUiDataBytes) {
        return 0;
    }
    const u64 at = used;
    std::memcpy(packet + Scene::UiDataOffset + at, reinterpret_cast<const void*>(from), bytes);
    used = (used + bytes + Alignment - 1) & ~(Alignment - 1);
    return address + Scene::UiDataOffset + at;
}

u64 Writer::CopyShared(VAddr from, u64 bytes) {
    for (u32 i = 0; i < RecentBuffers; i++) {
        if (recent_from[i] == from && recent_bytes[i] >= bytes) {
            return recent_to[i];
        }
    }
    const u64 to = Copy(from, bytes);
    recent_from[next_recent] = from;
    recent_bytes[next_recent] = bytes;
    recent_to[next_recent] = to;
    next_recent = (next_recent + 1) % RecentBuffers;
    return to;
}

// The inputs of one vertex usually lie in one buffer, so their range is copied once.
bool Writer::CopyInputs(VAddr table, u32 inputs, Scene::UiDraw& draw) {
    std::array<Buffer, Scene::UiAttributes> buffers;
    u64 low = ~u64{0};
    u64 high = 0;
    u64 total = 0;
    for (u32 i = 0; i < inputs; i++) {
        const auto words = Load<std::array<u32, 4>>(table + u64{i} * 16);
        buffers[i] = ReadBuffer(words.data());
        const u64 bytes = BufferBytes(buffers[i]);
        low = std::min(low, buffers[i].base);
        high = std::max(high, buffers[i].base + bytes);
        total += bytes;
    }
    const bool together = high - low <= total;
    const u64 whole = together ? CopyShared(low, high - low) : 0;
    for (u32 i = 0; i < inputs; i++) {
        const u64 copied = together ? (whole != 0 ? whole + (buffers[i].base - low) : 0)
                                    : CopyShared(buffers[i].base, BufferBytes(buffers[i]));
        if (copied == 0) {
            return false;
        }
        draw.attributes[i] = {
            .address = copied,
            .stride = buffers[i].stride,
            .format = buffers[i].word3,
        };
    }
    return true;
}

u32 Writer::TextureHandle(const u32* sharp, bool* linear) {
    const u64 base = SharpAddress(sharp);
    if (base == 0) {
        return 0;
    }
    for (u32 i = 0; i < g_target_count; i++) {
        if (g_targets[i].base == base) {
            return Scene::UiTargetHandles + i;
        }
    }
    bool dynamic = false;
    const u32 handle = Textures::GetBySharp(sharp, dynamic);
    if (linear != nullptr) {
        *linear = dynamic;
    }
    if (handle == 0) {
        ui.unknown_textures++;
        if (g_logged_textures < LoggedUnknowns &&
            std::find(g_unknown_textures.begin(), g_unknown_textures.end(), base) ==
                g_unknown_textures.end()) {
            g_unknown_textures[g_logged_textures++] = base;
            const std::span words{sharp, 8};
            LOG_INFO(Render_Vulkan, "GR2port UI: texture at {:#x} not ready: T# {:x}", base,
                     fmt::join(words, " "));
        }
        return NotReady;
    }
    const auto listed = g_dynamic.begin() + ui.dynamic;
    if (dynamic && ui.dynamic < g_dynamic.size() &&
        std::find(g_dynamic.begin(), listed, handle) == listed) {
        g_dynamic[ui.dynamic++] = handle;
    }
    return handle;
}

void Writer::AddRun(const Scene::UiRun& state) {
    if (ui.runs != 0) {
        Scene::UiRun& last = g_runs[ui.runs - 1];
        if (last.target == state.target && last.vertex == state.vertex &&
            last.pixel == state.pixel && last.blend == state.blend &&
            last.topology == state.topology &&
            std::equal(std::begin(last.viewport), std::end(last.viewport),
                       std::begin(state.viewport))) {
            last.count++;
            return;
        }
    }
    g_runs[ui.runs++] = state;
}

void Writer::Draw(u32 opcode, const u32* body) {
    const u64 vs = ProgramAddress(g_sh[ShPgmLoVs], g_sh[ShPgmHiVs]);
    const u64 ps = ProgramAddress(g_sh[ShPgmLoPs], g_sh[ShPgmHiPs]);
    if (vs == 0 || ps == 0) {
        ui.dropped++;
        return;
    }
    const u32 vs_hash = Load<u32>(vs + ProgramHash);
    const u32 ps_hash = Load<u32>(ps + ProgramHash);
    const auto vertex_program = std::ranges::find(VertexPrograms, vs_hash, &VertexProgram::hash);
    const auto pixel_program = std::ranges::find(PixelPrograms, ps_hash, &PixelProgram::hash);
    if (vertex_program == VertexPrograms.end() || pixel_program == PixelPrograms.end()) {
        ui.unported++;
        if (g_logged_programs < LoggedUnknowns) {
            g_logged_programs++;
            LOG_INFO(Render_Vulkan, "GR2port UI: programs not ported: vs {:#x} ps {:#x}", vs_hash,
                     ps_hash);
        }
        return;
    }
    const u32 primitive = g_uconfig[UconfigPrimitive];
    if (ui.draws == Scene::MaxUiDraws || ui.runs == Scene::MaxUiRuns ||
        (primitive != Scene::UiPrimitiveList && primitive != Scene::UiPrimitiveStrip)) {
        ui.dropped++;
        return;
    }
    const auto viewport_value = [](u32 reg) {
        return std::bit_cast<float>(g_context_registers[reg]);
    };
    const float scale_x = viewport_value(ContextViewportScaleX);
    const float scale_y = viewport_value(ContextViewportScaleY);
    const float offset_x = viewport_value(ContextViewportOffsetX);
    const float offset_y = viewport_value(ContextViewportOffsetY);
    const bool sized = g_target_width != 0;
    const u32 target =
        FindTarget(u64{g_context_registers[ContextColor0Base]} << 8,
                   sized ? g_target_width : static_cast<u32>(2.0f * std::abs(scale_x) + 0.5f),
                   sized ? g_target_height : static_cast<u32>(2.0f * std::abs(scale_y) + 0.5f));
    if (target == Scene::MaxUiTargets) {
        ui.dropped++;
        return;
    }

    u32 count = 0;
    Scene::UiDraw draw{};
    if (opcode == OpDrawIndexAuto) {
        count = body[0];
    } else {
        // DRAW_INDEX_OFFSET_2 reads the index buffer the engine set, DRAW_INDEX_2 its own.
        const bool offset = opcode == OpDrawIndexOffset2;
        const u64 indices = offset ? std::max<u64>(body[0], u64{body[1]} + body[2]) : body[3];
        count = offset ? body[2] : body[3];
        draw.first_index = offset ? body[1] : 0;
        draw.index_bits = g_index_bits;
        draw.indices = CopyShared(offset ? g_index_base : u64{body[1]} | u64{body[2]} << 32,
                                  indices * (g_index_bits / 8));
        if (draw.indices == 0) {
            ui.dropped++;
            return;
        }
    }

    const u32* const vs_user = &g_sh[ShUserDataVs];
    const u32* const ps_user = &g_sh[ShUserDataPs];
    if (vertex_program->inputs != 0 &&
        !CopyInputs(u64{vs_user[VsUserTable]} | u64{vs_user[VsUserTable + 1]} << 32,
                    vertex_program->inputs, draw)) {
        ui.dropped++;
        return;
    }
    const auto constants_of = [&](const u32* words) {
        const Buffer buffer = ReadBuffer(words);
        return Copy(buffer.base, u64{buffer.stride} * buffer.elements);
    };
    if (vertex_program->constants) {
        draw.vs_constants = constants_of(vs_user + VsUserConstants);
    }
    if (vertex_program->palette) {
        draw.vs_palette = constants_of(vs_user + VsUserPalette);
    }
    // Bit 0 clamps, bit 1 filters by point, as Ui's samplers.
    const auto sampler_of = [](const u32* words) {
        const u32 clamp = (words[0] & 7) != 0 ? 1 : 0;
        const u32 point = (words[2] >> 20 & 3) == 0 ? 2 : 0;
        return clamp | point;
    };
    const auto pixel_index = static_cast<u32>(pixel_program - PixelPrograms.begin());
    if (pixel_program->textures != 0) {
        bool linear = false;
        draw.textures[0] = TextureHandle(ps_user, &linear);
        draw.samplers[0] = sampler_of(ps_user + PsUserSampler);
        // The game writes text into linear images; movies and photos are linear too.
        if (linear && pixel_index != static_cast<u32>(Scene::UiPixelProgram::Movie) &&
            pixel_index != static_cast<u32>(Scene::UiPixelProgram::Photo)) {
            draw.flags |= Scene::UiDrawText;
            const auto vertex_index =
                static_cast<Scene::UiVertexProgram>(vertex_program - VertexPrograms.begin());
            if (vertex_index == Scene::UiVertexProgram::Transform ||
                vertex_index == Scene::UiVertexProgram::TransformB) {
                draw.flags |= Scene::UiDrawAnchored;
            }
        }
    }
    std::array<u32, 16> table_words{};
    if (pixel_program->textures == 2) {
        const VAddr table = u64{ps_user[PsUserConstants]} | u64{ps_user[PsUserConstants + 1]} << 32;
        table_words = Load<std::array<u32, 16>>(table);
        draw.textures[1] = TextureHandle(table_words.data());
        draw.samplers[1] = sampler_of(table_words.data() + TableSampler);
        draw.ps_constants = constants_of(table_words.data() + TableConstants);
    } else if (pixel_program->constants) {
        draw.ps_constants = constants_of(ps_user + PsUserConstants);
    }
    if (static_cast<u32>(pixel_program - PixelPrograms.begin()) ==
        static_cast<u32>(Scene::UiPixelProgram::Photo)) {
        LOG_INFO(Render_Vulkan,
                 "GR2port photo: drawn into target {}, the frame from {:#x} as {:#x}, the filter's "
                 "frame from {:#x} as {:#x}, curves of {} bytes at {:#x}; world target {}",
                 target, SharpAddress(ps_user), draw.textures[0], SharpAddress(table_words.data()),
                 draw.textures[1],
                 u64{ReadBuffer(table_words.data() + TableConstants).stride} *
                     ReadBuffer(table_words.data() + TableConstants).elements,
                 draw.ps_constants, ui.world);
    }
    // A texture the renderer does not have yet reads white, which a movie's colour conversion
    // turns pink, so the draw is left out until the renderer has it.
    if (draw.textures[0] == NotReady || draw.textures[1] == NotReady) {
        return;
    }

    AddRun({
        .target = target,
        .vertex = static_cast<u32>(vertex_program - VertexPrograms.begin()),
        .pixel = static_cast<u32>(pixel_program - PixelPrograms.begin()),
        .blend = g_context_registers[ContextBlend0],
        .topology = primitive,
        .viewport = {offset_x - scale_x, offset_y - scale_y, 2.0f * scale_x, 2.0f * scale_y},
        .first = ui.draws,
        .count = 1,
    });
    g_commands[ui.draws] = {
        .vertex_count = count,
        .instance_count = 1,
        .first_vertex = 0,
        .first_instance = ui.draws,
    };
    std::memcpy(packet + Scene::UiDrawsOffset + u64{ui.draws} * sizeof(draw), &draw, sizeof(draw));
    ui.draws++;
}

void Writer::Finish() {
    ui.targets = g_target_count;
    std::memcpy(packet + Scene::UiTargetsOffset, g_targets.data(),
                g_target_count * sizeof(Scene::UiTarget));
    std::memcpy(packet + Scene::UiRunsOffset, g_runs.data(), ui.runs * sizeof(Scene::UiRun));
    std::memcpy(packet + Scene::UiCommandsOffset, g_commands.data(),
                ui.draws * sizeof(Scene::VertexCommand));
    std::memcpy(packet + Scene::UiDynamicOffset, g_dynamic.data(), ui.dynamic * sizeof(u32));
}

void Scan(const u32* at, const u32* stop, Writer& writer) {
    g_words += static_cast<u64>(stop - at);
    while (at < stop) {
        const u32 header = *at;
        const u32 type = header >> 30;
        if (type == PacketType2) {
            at++;
            continue;
        }
        const u32 count = header >> 16 & 0x3fff;
        if (type == PacketType3) {
            const u32 opcode = header >> 8 & 0xff;
            const u32* const body = at + 1;
            switch (opcode) {
            case OpSetShReg:
                Store(g_sh, body, count);
                break;
            case OpSetContextReg: {
                Store(g_context_registers, body, count);
                const u32 first = body[0] & 0xffff;
                if (first <= ContextColor0Base && ContextColor0Base < first + count) {
                    g_target_width = 0;
                    g_target_height = 0;
                } else if (first == ContextScreenScissor && count >= 2 && g_target_width == 0) {
                    g_target_width = body[2] & 0xffff;
                    g_target_height = body[2] >> 16;
                }
                break;
            }
            case OpSetUconfigReg:
                Store(g_uconfig, body, count);
                break;
            case OpIndexBase:
                g_index_base = u64{body[0]} | u64{body[1] & 0xffff} << 32;
                break;
            case OpIndexType:
                g_index_bits = (body[0] & 3) != 0 ? 32 : 16;
                break;
            case OpDrawIndexAuto:
            case OpDrawIndexOffset2:
            case OpDrawIndex2:
                writer.Draw(opcode, body);
                break;
            default:
                break;
            }
        }
        at += count + 2;
    }
}

} // namespace

void Install(VAddr module_base) {
    g_base = module_base;
}

void OnFrameGraphEnd() {
    const u32 index = Load<u32>(g_base + ContextIndex - GoldHenBase);
    g_context = g_base + Contexts - GoldHenBase + u64{index} * ContextBytes;
    g_start = Load<VAddr>(g_context + StreamNext);
    // The state the screen drawing does not send again.
    const auto viewport = Load<std::array<s32, 4>>(g_context + ContextViewport);
    const float half_width = 0.5f * static_cast<float>(viewport[2]);
    const float half_height = 0.5f * static_cast<float>(viewport[3]);
    g_context_registers[ContextViewportScaleX] = std::bit_cast<u32>(half_width);
    g_context_registers[ContextViewportOffsetX] =
        std::bit_cast<u32>(static_cast<float>(viewport[0]) + half_width);
    g_context_registers[ContextViewportScaleY] = std::bit_cast<u32>(-half_height);
    g_context_registers[ContextViewportOffsetY] =
        std::bit_cast<u32>(static_cast<float>(viewport[1]) + half_height);
    u32 blend = 0;
    if (Load<u8>(g_context + ContextBlendOn) != 0) {
        const auto ops = Load<std::array<u32, 2>>(g_context + ContextBlendOps);
        const auto factors = Load<std::array<u32, 4>>(g_context + ContextBlendFactors);
        constexpr u32 SeparateAlpha = 1u << 29;
        constexpr u32 Enable = 1u << 30;
        blend = factors[0] | ops[0] << 5 | factors[1] << 8 | factors[2] << 16 | ops[1] << 21 |
                factors[3] << 24 | SeparateAlpha | Enable;
    }
    g_context_registers[ContextBlend0] = blend;
    g_uconfig[UconfigPrimitive] = Load<u32>(g_context + ContextPrimitive);
    g_index_bits = Load<u32>(g_context + ContextIndexType) != 0 ? 32 : 16;
    g_index_base = Load<VAddr>(g_context + ContextIndexBase);
}

void Skip() {
    g_context = 0;
}

void Write(u8* packet, u64 address, Scene::Header& header, VAddr world) {
    const auto start = std::chrono::steady_clock::now();
    const u32 index = Load<u32>(g_base + PreviousContextIndex - GoldHenBase);
    const VAddr context = g_base + Contexts - GoldHenBase + u64{index} * ContextBytes;
    const VAddr begin = Load<VAddr>(context + StreamBegin);
    const VAddr end = Load<VAddr>(context + StreamEnd);
    const VAddr next = Load<VAddr>(context + StreamNext);
    const VAddr from = context == g_context ? g_start : begin;
    g_world_frames += context == g_context ? 1 : 0;
    g_context = 0;

    Scene::UiHeader& ui = header.ui;
    const u32 picture =
        ObjectTarget(g_base + BackBuffers - GoldHenBase + u64{index} * BackBufferBytes);
    ui.picture = picture == Scene::MaxUiTargets ? 0 : picture;
    const u32 world_target = world != 0 ? ObjectTarget(world) : ui.picture;
    ui.world = world_target == Scene::MaxUiTargets ? ui.picture : world_target;
    Writer writer{packet, address, ui};
    if (begin != 0 && from >= begin && from <= next && next <= end) {
        Scan(reinterpret_cast<const u32*>(from), reinterpret_cast<const u32*>(next), writer);
    } else {
        g_unread++;
    }
    writer.Finish();
    ui.photo = PhotoTarget(ui);

    g_draws += ui.draws;
    g_runs_total += ui.runs;
    g_bytes += writer.Used();
    g_unknown += ui.unknown_textures;
    g_unported += ui.unported;
    g_dropped += ui.dropped;
    g_write_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
    if (++g_frames == ReportFrames) {
        constexpr double Frames = ReportFrames;
        LOG_INFO(Render_Vulkan,
                 "GR2port UI: per frame {:.1f} draws in {:.1f} runs, {:.0f} command words, {:.0f} "
                 "bytes copied, {:.1f} us to write; {:.1f} reads of textures not ready, {:.1f} "
                 "draws not ported, {:.1f} dropped; {} frames unread, {} with the frame graph, {} "
                 "targets",
                 static_cast<double>(g_draws) / Frames, static_cast<double>(g_runs_total) / Frames,
                 static_cast<double>(g_words) / Frames, static_cast<double>(g_bytes) / Frames,
                 static_cast<double>(g_write_ns) * 1e-3 / Frames,
                 static_cast<double>(g_unknown) / Frames, static_cast<double>(g_unported) / Frames,
                 static_cast<double>(g_dropped) / Frames, g_unread, g_world_frames, g_target_count);
        g_frames = 0;
        g_world_frames = 0;
        g_draws = 0;
        g_runs_total = 0;
        g_words = 0;
        g_bytes = 0;
        g_unknown = 0;
        g_unported = 0;
        g_dropped = 0;
        g_write_ns = 0;
        g_unread = 0;
    }
}

} // namespace Gr2Port::UiCapture
