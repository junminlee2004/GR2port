// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <span>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/kernel/process.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/gr2port/submits.h"

namespace Gr2Port::Submits {

namespace {

using AmdGpu::PM4Header;
using AmdGpu::PM4ItOpcode;

// The value an occlusion query reads back, counted up so that every query finds pixels drawn.
u64 g_pixel_counter = 0;

void WriteLabel(void* address, u64 data, u32 num_bytes) {
    ASSERT(Core::Memory::Instance()->TryWriteBacking(address, &data, num_bytes));
}

void WriteData(const PM4Header* header) {
    const auto* write_data = reinterpret_cast<const AmdGpu::PM4CmdWriteData*>(header);
    ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
    ASSERT(!write_data->wr_one_addr.Value());
    const u32 data_size = (header->type3.count.Value() - 2) * 4;
    std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
}

// A wait finds the semaphore as it is: nothing runs later to signal it.
void MemSemaphore(const PM4Header* header) {
    const auto* mem_semaphore = reinterpret_cast<const AmdGpu::PM4CmdMemSemaphore*>(header);
    if (mem_semaphore->IsSignaling()) {
        mem_semaphore->Signal();
    } else if (mem_semaphore->Signaled()) {
        mem_semaphore->Decrement();
    }
}

void EventWrite(const PM4Header* header) {
    const auto* event = reinterpret_cast<const AmdGpu::PM4CmdEventWrite*>(header);
    if (event->event_index.Value() != AmdGpu::EventIndex::ZpassDone ||
        event->event_type.Value() != AmdGpu::EventType::PixelPipeStatDump) {
        return;
    }
    static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
    static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
    static const s32 num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    u64* results = event->Address<u64*>();
    for (s32 i = 0; i < num_counter_pairs; ++i, results += 2) {
        *results = g_pixel_counter | OcclusionCounterValidMask;
    }
    g_pixel_counter += OcclusionCounterStep;
}

void Advance(Ring& ring, u32 dwords) {
    *ring.read_pointer = (*ring.read_pointer + dwords) % ring.size_dwords;
}

// If indirect is true, the packets are in a buffer the ring points to, and the ring's read pointer
// stays.
void WalkCompute(Ring& ring, std::span<const u32> acb, bool indirect) {
    while (!acb.empty()) {
        const auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 dwords = header->type3.NumWords() + 1;

        const bool resumed = !ring.held.empty();
        if (resumed) {
            const u32 total =
                reinterpret_cast<const PM4Header*>(ring.held.data())->type3.NumWords() + 1;
            dwords = total - static_cast<u32>(ring.held.size());
        }
        if (dwords > acb.size()) {
            ring.held.insert(ring.held.end(), acb.begin(), acb.end());
            if (!indirect) {
                Advance(ring, static_cast<u32>(acb.size()));
            }
            return;
        }
        if (resumed) {
            ring.held.insert(ring.held.end(), acb.begin(), acb.begin() + dwords);
            header = reinterpret_cast<const PM4Header*>(ring.held.data());
        }

        if (header->type == 2) {
            // Padding.
            dwords = 1;
        } else {
            ASSERT_MSG(header->type == 3, "Invalid PM4 type {}", header->type.Value());
            switch (header->type3.opcode) {
            case PM4ItOpcode::IndirectBuffer: {
                const auto* indirect_buffer =
                    reinterpret_cast<const AmdGpu::PM4CmdIndirectBuffer*>(header);
                const std::span<const u32> buffer{indirect_buffer->Address<const u32>(),
                                                  indirect_buffer->ib_size};
                if (indirect && indirect_buffer->chain) {
                    // A chained buffer replaces the current one.
                    acb = buffer;
                    continue;
                }
                WalkCompute(ring, buffer, true);
                break;
            }
            case PM4ItOpcode::WriteData:
                WriteData(header);
                break;
            case PM4ItOpcode::MemSemaphore:
                MemSemaphore(header);
                break;
            case PM4ItOpcode::ReleaseMem: {
                const auto* release_mem = reinterpret_cast<const AmdGpu::PM4CmdReleaseMem*>(header);
                const auto irq = static_cast<Platform::InterruptId>(ring.pipe);
                release_mem->SignalFence([irq] { Platform::IrqC::Instance()->Signal(irq); },
                                         [](VAddr, u16, u16) {});
                break;
            }
            default:
                break;
            }
        }
        if (resumed) {
            ring.held.clear();
        }

        acb = acb.subspan(dwords);
        if (!indirect) {
            Advance(ring, dwords);
        }
    }
}

} // namespace

void Graphics(std::span<const u32> dcb) {
    while (!dcb.empty()) {
        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        if (header->type != 3) {
            dcb = dcb.subspan(1);
            continue;
        }
        const u32 dwords = header->type3.NumWords() + 1;
        if (dwords > dcb.size()) {
            LOG_ERROR(Render_Vulkan, "GR2port: a packet of {} dwords with {} left in its buffer",
                      dwords, dcb.size());
            return;
        }

        switch (header->type3.opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const AmdGpu::PM4CmdNop*>(header);
            if (nop->header.count.Value() != 0 &&
                nop->data_block[0] == AmdGpu::PM4CmdNop::PayloadType::PatchedFlip) {
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
            }
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            const auto* indirect_buffer =
                reinterpret_cast<const AmdGpu::PM4CmdIndirectBuffer*>(header);
            const std::span<const u32> buffer{indirect_buffer->Address<const u32>(),
                                              indirect_buffer->ib_size};
            if (indirect_buffer->chain) {
                // A chained buffer replaces the current one.
                dcb = buffer;
                continue;
            }
            Graphics(buffer);
            break;
        }
        case PM4ItOpcode::EventWrite:
            EventWrite(header);
            break;
        case PM4ItOpcode::EventWriteEos:
            reinterpret_cast<const AmdGpu::PM4CmdEventWriteEos*>(header)->SignalFence(WriteLabel);
            break;
        case PM4ItOpcode::EventWriteEop:
            reinterpret_cast<const AmdGpu::PM4CmdEventWriteEop*>(header)->SignalFence(
                WriteLabel,
                [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
            break;
        case PM4ItOpcode::WriteData:
            WriteData(header);
            break;
        case PM4ItOpcode::MemSemaphore:
            MemSemaphore(header);
            break;
        default:
            break;
        }
        dcb = dcb.subspan(dwords);
    }
}

void Compute(Ring& ring, std::span<const u32> acb) {
    WalkCompute(ring, acb, false);
}

} // namespace Gr2Port::Submits
