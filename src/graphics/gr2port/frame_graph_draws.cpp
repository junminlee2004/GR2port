// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/frame_graph_draws.h"
#include "graphics/gr2port/hooks.h"

namespace Gr2Port::FrameGraphDraws {

namespace {

// SubmitCategory FUN_014214e0, the frame graph's per-item draw loop. Its handlers read the items
// and write GNM commands, which nothing reads.
constexpr Hooks::Entry DrawLoop{0x14214e0, 6, Hooks::FramePrologue};

// The engine's drawing above the binders, which builds only commands once they return: the draw
// calls FUN_016173f0 (indexed) and FUN_01617340, and with them their state commit FUN_01614150;
// the target setter FUN_016137a0; the program configuration FUN_01616440 and the pixel program
// FUN_0042f150; the constant builders of the 2D shader objects (their virtual function at +0x38),
// the strokes' geometry program FUN_017b6430, and the bind item's function FUN_013ecc20, whose
// buffers UiItems reads from the item. UiItems reads the screen drawing from its items, and the
// frame graph's draw loop already returns.
constexpr std::array Drawing{
    Hooks::Entry{0x16173f0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x1617340, 6, Hooks::FramePrologue},
    Hooks::Entry{0x16137a0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x1616440, 6, Hooks::FramePrologueR14},
    // push rbx; test rsi, rsi; setne [rdi + 0xc1d1].
    Hooks::Entry{0x42f150, 11, {0x53, 0x48, 0x85, 0xf6, 0x0f, 0x95, 0x87, 0xd1, 0xc1, 0x00, 0x00}},
    Hooks::Entry{0x17b59b0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b5c50, 6, Hooks::FramePrologueR14},
    Hooks::Entry{0x17b5d60, 6, Hooks::FramePrologueR14},
    Hooks::Entry{0x17b5fc0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b6a40, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b6e00, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b71b0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b7740, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b7a10, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b7b20, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b7c20, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b7d50, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b7f00, 6, Hooks::FramePrologue},
    Hooks::Entry{0x17b6430, 6, Hooks::FramePrologue},
    Hooks::Entry{0x13ecc20, 6, Hooks::FramePrologue},
};

// What the game calls for each draw and dispatch on a graphics context, which write only command
// buffer data and packets, so they return at once: FUN_0042dc50 binds a draw's shaders and writes
// their constants, and FUN_0042da70 does the same for a dispatch.
constexpr std::array Binders{
    Hooks::Entry{0x42dc50, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42da70, 6, Hooks::FramePrologue},
    // The binders copy a stage's constant buffers, buffers, read-write buffers, textures,
    // read-write textures and samplers for its next draw on a graphics context, then for the next
    // dispatch on a compute queue.
    Hooks::Entry{0x42f410, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42f690, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42f7d0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42f910, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42fa50, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42fb90, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42d1d0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42d290, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42d350, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42d410, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42d4d0, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42d590, 6, Hooks::FramePrologue},
};

// FUN_00414c00(commands, bytes, alignment) takes room in a command buffer for bytes of data,
// aligned to 2 to the power of alignment bytes, and returns it: a label's slot, or constants a
// binder points a program at. The game's function writes a NOP over the room, which nothing reads,
// so the room holds the data alone.
constexpr Hooks::Entry Allocator{0x414c00, 6, Hooks::FramePrologue};

// The head of a command buffer of the game: its first word, its end, its next free word, and the
// function that makes room for a count of words, with the argument it takes.
struct CommandBuffer {
    u32* begin;
    u32* end;
    u32* next;
    u8 PS4_SYSV_ABI (*make_room)(CommandBuffer* commands, u32 count, u64 argument);
    u64 argument;
};

// push rbp; mov rbp, rsp.
constexpr std::array<u8, 11> PushFrame{0x55, 0x48, 0x89, 0xe5};

// Functions that write packets which nothing reads, so they return at once, as UiCapture makes the
// writers of the screen drawing's programs, registers and draws return: a compute queue's
// dispatch, FUN_0042cfb0 and FUN_00413560, the functions that reserve room for one packet, call
// the driver and advance, and the thunks of two driver functions whose results the game leaves
// unused.
constexpr std::array Returning{
    Hooks::Entry{0x42cfb0, 4, PushFrame},
    Hooks::Entry{0x413560, 4, PushFrame},
    Hooks::Entry{0x4135d0, 4, PushFrame}, // sceGnmDispatchDirect
    Hooks::Entry{0x429510, 4, PushFrame}, // sceGnmDispatchInitDefaultHardwareState
    Hooks::Entry{0x410e50, 4, PushFrame}, // sceGnmDrawInitDefaultHardwareState350
    Hooks::Entry{0x412d60, 4, PushFrame}, // sceGnmDrawIndirect
    Hooks::Entry{0x416c10, 4, PushFrame}, // sceGnmInsertPushMarker
    Hooks::Entry{0x416d10, 4, PushFrame}, // sceGnmInsertPopMarker
    Hooks::Entry{0x417070, 4, PushFrame}, // sceGnmInsertWaitFlipDone
    Hooks::Entry{0x4141e0, 4, PushFrame}, // sceGnmSetCsShaderWithModifier
    Hooks::Entry{0x413e20, 4, PushFrame}, // sceGnmSetEmbeddedVsShader
    Hooks::Entry{0x414510, 4, PushFrame}, // sceGnmSetGsShader
    Hooks::Entry{0x414570, 4, PushFrame}, // sceGnmUpdateGsShader
    Hooks::Entry{0x413f70, 4, PushFrame}, // sceGnmSetHsShader
    Hooks::Entry{0x413fe0, 4, PushFrame}, // sceGnmUpdateHsShader
    Hooks::Entry{0x414170, 4, PushFrame}, // sceGnmSetLsShader
    Hooks::Entry{0x414430, 4, PushFrame}, // sceGnmSetVgtControl
    Hooks::Entry{0x4144a0, 4, PushFrame}, // sceGnmSetVgtControl
    Hooks::Entry{0x429130, 5, {0xe9, 0xfb, 0xee, 0x3c, 0x01}}, // sceGnmFlushGarlic
    Hooks::Entry{0x4291b0, 5, {0xe9, 0xcb, 0xec, 0x3c, 0x01}}, // sceGnmSubmitDone
    // A compute command buffer's cache invalidation and shader control, and its submit to a
    // compute queue, whose result the game leaves unused. Nothing runs the compute queues.
    Hooks::Entry{0x429b40, 4, PushFrame},
    Hooks::Entry{0x4112d0, 4, PushFrame},
    Hooks::Entry{0x42ab60, 4, PushFrame},
    // A graphics command buffer's cache flushes and waits, among them the frame's first flush and
    // its last wait, and the room the frame leaves at its end for flip packets, which the submit
    // does not write.
    Hooks::Entry{0x414950, 4, PushFrame},
    Hooks::Entry{0x416730, 4, PushFrame},
    Hooks::Entry{0x416b40, 4, PushFrame},
    Hooks::Entry{0x4170d0, 4, PushFrame},
    // The writers of other registers, which the frame graph's passes and the screen drawing
    // send: the registers of the draw wrappers' state flush FUN_01614150, the targets and
    // viewports of FUN_016137a0, FUN_01616ee0, FUN_0161b3e0 and FUN_0161b760, the index buffer's
    // size of FUN_01616e10, the pipeline events, the shader stages of FUN_00414b70, and the pixel
    // inputs and the DMA of constants of FUN_0042dc50. Each writes its packet and nothing else.
    Hooks::Entry{0x411550, 4, PushFrame},
    Hooks::Entry{0x411680, 4, PushFrame},
    Hooks::Entry{0x411710, 4, PushFrame},
    Hooks::Entry{0x4118a0, 4, PushFrame},
    Hooks::Entry{0x411920, 4, PushFrame},
    Hooks::Entry{0x411980, 4, PushFrame},
    Hooks::Entry{0x4119e0, 4, PushFrame},
    Hooks::Entry{0x411a40, 4, PushFrame},
    Hooks::Entry{0x411aa0, 4, PushFrame},
    Hooks::Entry{0x411b10, 4, PushFrame},
    Hooks::Entry{0x411b70, 4, PushFrame},
    Hooks::Entry{0x411bd0, 4, PushFrame},
    Hooks::Entry{0x411c50, 4, PushFrame},
    Hooks::Entry{0x411e50, 4, PushFrame},
    Hooks::Entry{0x412410, 4, PushFrame},
    Hooks::Entry{0x412590, 4, PushFrame},
    Hooks::Entry{0x413800, 4, PushFrame},
    Hooks::Entry{0x413bf0, 4, PushFrame},
    Hooks::Entry{0x4148e0, 4, PushFrame},
    Hooks::Entry{0x414b70, 4, PushFrame},
    Hooks::Entry{0x4154b0, 4, PushFrame},
    Hooks::Entry{0x415690, 4, PushFrame},
    Hooks::Entry{0x415b10, 4, PushFrame},
    Hooks::Entry{0x415b70, 4, PushFrame},
    Hooks::Entry{0x415bd0, 4, PushFrame},
    Hooks::Entry{0x415d10, 4, PushFrame},
    Hooks::Entry{0x415d80, 4, PushFrame},
    Hooks::Entry{0x415df0, 4, PushFrame},
    Hooks::Entry{0x415e60, 4, PushFrame},
    Hooks::Entry{0x415ed0, 4, PushFrame},
    Hooks::Entry{0x415f50, 4, PushFrame},
    // The frame graph's depth sort orders the G2_BG_Main keys for the draw loop. Capture reads the
    // keys when the frame graph starts.
    Hooks::Entry{0x1441460, 4, PushFrame},
    // The particle draw passes bind and draw the draw list that ParticleCapture reads when
    // FUN_014a3ba0 returns.
    Hooks::Entry{0x14baaa0, 4, PushFrame},
    // The frame graph's binders of constants and textures copy constants into the command buffer
    // and set a texture's memory type and sampler words for its programs.
    Hooks::Entry{0x16167f0, 4, PushFrame},
    Hooks::Entry{0x1616c10, 4, PushFrame},
    // The particle compute passes dispatch the game's simulation programs and signal the labels
    // the graphics context waits for. ParticleCapture reads the simulation's input.
    Hooks::Entry{0x14a4a10, 4, PushFrame},
    Hooks::Entry{0x14a7350, 4, PushFrame},
    // The movie job's compute user data and its keep's DMA copy: Movies reads the job and the keep
    // from the player's objects.
    Hooks::Entry{0x429820, 6, Hooks::FramePrologue},
    Hooks::Entry{0x42bf10, 6, Hooks::FramePrologue},
    // The frame graph's deformer passes, FUN_01421700 for each kind: the begin, record and end
    // functions of a kind bind and dispatch its programs and write the words of the dispatch into
    // the constants the record's builder took for this frame, which nothing reads. Capture reads
    // the records when the frame graph starts.
    Hooks::Entry{0x1421700, 6, Hooks::FramePrologue},
};

// The draw thread sorts the frame's item keys and its deformer list with FUN_017b9330(first, last,
// depth), the standard library's sort of u32 words. A radix sort leaves the same words in the
// same order, since words that compare equal are the same.
constexpr Hooks::Entry SortWords{0x17b9330, 6, Hooks::FramePrologue};
// Below this many words the standard library's sort is the quicker.
constexpr size_t RadixSortWords = 256;

// In place of FUN_00414c00: the room from the buffer's next free word, or null if the buffer has
// none.
u32* PS4_SYSV_ABI OnAllocate(CommandBuffer* commands, u32 bytes, u32 alignment) {
    const u64 words = (u64{bytes} + 3) / sizeof(u32);
    const u64 align = u64{1} << (alignment & 31);
    const auto start = [&] {
        return (reinterpret_cast<VAddr>(commands->next) + align - 1) & ~(align - 1);
    };
    VAddr data = start();
    if (data + words * sizeof(u32) > reinterpret_cast<VAddr>(commands->end)) {
        if (Core::Recompiled::Call(commands->make_room, commands,
                                   static_cast<u32>(words + align / sizeof(u32)),
                                   commands->argument) == 0) {
            return nullptr;
        }
        data = start();
    }
    commands->next = reinterpret_cast<u32*>(data) + words;
    return reinterpret_cast<u32*>(data);
}

// In place of FUN_017b9330: sorts the words from first to last a byte at a time, from the lowest,
// passing over a byte that all of them share.
void PS4_SYSV_ABI OnSortWords(u32* first, u32* last, s64) {
    const size_t count = static_cast<size_t>(last - first);
    if (count < RadixSortWords) {
        std::sort(first, last);
        return;
    }
    thread_local std::vector<u32> scratch;
    if (scratch.size() < count) {
        scratch.resize(count);
    }
    std::array<std::array<u32, 256>, 4> places{};
    for (size_t i = 0; i < count; i++) {
        const u32 word = first[i];
        places[0][word & 0xff]++;
        places[1][(word >> 8) & 0xff]++;
        places[2][(word >> 16) & 0xff]++;
        places[3][word >> 24]++;
    }
    u32* in = first;
    u32* out = scratch.data();
    for (u32 byte = 0; byte < places.size(); byte++) {
        const u32 shift = byte * 8;
        std::array<u32, 256>& place = places[byte];
        if (place[(in[0] >> shift) & 0xff] == count) {
            continue;
        }
        u32 next = 0;
        for (u32& words : place) {
            next += std::exchange(words, next);
        }
        for (size_t i = 0; i < count; i++) {
            const u32 word = in[i];
            out[place[(word >> shift) & 0xff]++] = word;
        }
        std::swap(in, out);
    }
    if (in != first) {
        std::memcpy(first, in, count * sizeof(u32));
    }
}

} // namespace

void Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    if (Hooks::SetReturning(module_base, DrawLoop, true) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the draw loop is not the expected code");
    }
    for (const std::span<const Hooks::Entry> entries :
         {std::span<const Hooks::Entry>{Drawing}, std::span<const Hooks::Entry>{Binders},
          std::span<const Hooks::Entry>{Returning}}) {
        for (const Hooks::Entry& entry : entries) {
            if (Hooks::Return(module_base, entry) != Hooks::Result::Hooked) {
                LOG_ERROR(Render_Vulkan, "GR2port: the function at {:#x} is not the expected code",
                          entry.address);
            }
        }
    }
    if (Hooks::Replace(module_base, Allocator, c, capacity,
                       reinterpret_cast<const void*>(&OnAllocate)) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the function at {:#x} is not the expected code",
                  Allocator.address);
    }
    if (Hooks::Replace(module_base, SortWords, c, capacity,
                       reinterpret_cast<const void*>(&OnSortWords)) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the function at {:#x} is not the expected code",
                  SortWords.address);
    }
}

} // namespace Gr2Port::FrameGraphDraws
