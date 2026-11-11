// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ngs2.h"
#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_mixer.h"

#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/kernel/kernel.h"

using namespace Libraries::Kernel;

namespace Libraries::Ngs2 {

// A handle is the address of a slot in the buffer the game gives for a system or a rack. The game
// keeps a handle in 40 bits, which an address in its own memory fits.
struct Slot {
    Slot* self;
    void* object;
    u32 type;
    u32 index;
};

// The start of the buffer of a system or a rack. The slots of the voices of a rack follow it.
struct Block {
    Slot handle;
    OrbisNgs2ContextBufferInfo buffer;
    OrbisNgs2BufferFreeHandler free;
};

static size_t BlockSize(u32 numVoices) {
    return sizeof(Block) + numVoices * sizeof(Slot) + alignof(Block);
}

static Block* BlockOf(const OrbisNgs2ContextBufferInfo& buffer) {
    return reinterpret_cast<Block*>(
        Common::AlignUp(reinterpret_cast<uintptr_t>(buffer.hostBuffer), alignof(Block)));
}

static Slot* VoiceSlots(OrbisNgs2Handle rackHandle) {
    return reinterpret_cast<Slot*>(reinterpret_cast<Block*>(rackHandle) + 1);
}

static u32 RackVoices(const OrbisNgs2RackOption* option) {
    return option ? option->maxVoices : 1;
}

static void* FromHandle(OrbisNgs2Handle handle, OrbisNgs2HandleType type) {
    const Slot* slot = reinterpret_cast<const Slot*>(handle);
    if (!slot || slot->self != slot || slot->type != static_cast<u32>(type)) {
        return nullptr;
    }
    return slot->object;
}

Mixer::System* SystemFromHandle(OrbisNgs2Handle handle) {
    return static_cast<Mixer::System*>(FromHandle(handle, OrbisNgs2HandleType::System));
}

Mixer::Rack* RackFromHandle(OrbisNgs2Handle handle) {
    return static_cast<Mixer::Rack*>(FromHandle(handle, OrbisNgs2HandleType::Rack));
}

Mixer::Voice* VoiceFromHandle(OrbisNgs2Handle handle) {
    return static_cast<Mixer::Voice*>(FromHandle(handle, OrbisNgs2HandleType::Voice));
}

s32 HandleReportInvalid(OrbisNgs2Handle handle, u32 handleType) {
    switch (handleType) {
    case 1:
        LOG_ERROR(Lib_Ngs2, "Invalid system handle {}", handle);
        return ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
    case 2:
        LOG_ERROR(Lib_Ngs2, "Invalid rack handle {}", handle);
        return ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
    case 4:
        LOG_ERROR(Lib_Ngs2, "Invalid voice handle {}", handle);
        return ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
    case 8:
        LOG_ERROR(Lib_Ngs2, "Invalid report handle {}", handle);
        return ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
    default:
        LOG_ERROR(Lib_Ngs2, "Invalid handle {}", handle);
        return ORBIS_NGS2_ERROR_INVALID_HANDLE;
    }
}

void* MemoryClear(void* buffer, size_t size) {
    return memset(buffer, 0, size);
}

s32 StackBufferClose(StackBuffer* stackBuffer, size_t* outTotalSize) {
    if (outTotalSize) {
        *outTotalSize = stackBuffer->usedSize + stackBuffer->alignment;
    }
    return ORBIS_OK;
}

s32 StackBufferOpen(StackBuffer* stackBuffer, void* bufferStart, size_t bufferSize,
                    void** outBuffer, u8 flags) {
    stackBuffer->top = outBuffer;
    stackBuffer->base = bufferStart;
    stackBuffer->size = (size_t)bufferStart;
    stackBuffer->currentOffset = (size_t)bufferStart;
    stackBuffer->usedSize = 0;
    stackBuffer->totalSize = bufferSize;
    stackBuffer->alignment = 8; // this is a fixed value
    stackBuffer->flags = flags;

    if (outBuffer != NULL) {
        *outBuffer = NULL;
    }

    return ORBIS_OK;
}

s32 SystemCleanup(OrbisNgs2Handle systemHandle, OrbisNgs2ContextBufferInfo* outInfo) {
    Mixer::System* system = SystemFromHandle(systemHandle);
    if (!system) {
        return HandleReportInvalid(systemHandle, 1);
    }
    Block* block = reinterpret_cast<Block*>(systemHandle);
    OrbisNgs2ContextBufferInfo buffer = block->buffer;
    const OrbisNgs2BufferFreeHandler hostFree = block->free;
    Mixer::DestroySystem(system);
    // The handle names nothing from here on.
    MemoryClear(block, sizeof(Block));
    if (hostFree) {
        hostFree(&buffer);
    }
    if (outInfo) {
        *outInfo = buffer;
    }
    return ORBIS_OK;
}

s32 SystemSetupCore(StackBuffer* stackBuffer, const OrbisNgs2SystemOption* option,
                    SystemInternal* outSystem) {
    u32 maxGrainSamples = 512;
    u32 numGrainSamples = 256;
    u32 sampleRate = 48000;

    if (option) {
        sampleRate = option->sampleRate;
        maxGrainSamples = option->maxGrainSamples;
        numGrainSamples = option->numGrainSamples;
    }

    if (maxGrainSamples < 64 || maxGrainSamples > 1024 || (maxGrainSamples & 63) != 0) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option (maxGrainSamples={},x64)", maxGrainSamples);
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    }

    if (numGrainSamples < 64 || numGrainSamples > 1024 || (numGrainSamples & 63) != 0) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option (numGrainSamples={},x64)", numGrainSamples);
        return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    }

    if (sampleRate != 11025 && sampleRate != 12000 && sampleRate != 22050 && sampleRate != 24000 &&
        sampleRate != 44100 && sampleRate != 48000 && sampleRate != 88200 && sampleRate != 96000 &&
        sampleRate != 176400 && sampleRate != 192000) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option(sampleRate={}:44.1/48kHz series)", sampleRate);
        return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    }

    if (outSystem) {
        // dummy handle
        outSystem->systemHandle = 1;
    }

    return ORBIS_OK;
}

s32 SystemSetup(const OrbisNgs2SystemOption* option, OrbisNgs2ContextBufferInfo* hostBufferInfo,
                OrbisNgs2BufferFreeHandler hostFree, OrbisNgs2Handle* outHandle) {
    if (option && option->size != 64) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option size ({})", option->size);
        return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
    }

    const s32 result = SystemSetupCore(nullptr, option, nullptr);
    if (result < 0) {
        return result;
    }

    const size_t requiredBufferSize = BlockSize(0);

    // outHandle unprovided
    if (!outHandle) {
        hostBufferInfo->hostBuffer = NULL;
        hostBufferInfo->hostBufferSize = requiredBufferSize;
        MemoryClear(&hostBufferInfo->reserved, sizeof(hostBufferInfo->reserved));
        return ORBIS_OK;
    }

    if (!hostBufferInfo->hostBuffer) {
        LOG_ERROR(Lib_Ngs2, "Invalid system buffer address ({})", hostBufferInfo->hostBuffer);
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    }

    if (hostBufferInfo->hostBufferSize < requiredBufferSize) {
        LOG_ERROR(Lib_Ngs2, "Invalid system buffer size ({}<{}[byte])",
                  hostBufferInfo->hostBufferSize, requiredBufferSize);
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE;
    }

    // Setup
    Block* block = BlockOf(*hostBufferInfo);
    Mixer::System* system = Mixer::CreateSystem(option ? option->sampleRate : 48000,
                                                option ? option->numGrainSamples : 256);
    block->handle = {&block->handle, system, static_cast<u32>(OrbisNgs2HandleType::System), 0};
    block->buffer = *hostBufferInfo;
    block->free = hostFree;
    *outHandle = reinterpret_cast<OrbisNgs2Handle>(block);
    return ORBIS_OK;
}

s32 RackQueryBufferSize(const OrbisNgs2RackOption* option,
                        OrbisNgs2ContextBufferInfo* outBufferInfo) {
    if (!outBufferInfo) {
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    }
    if (option && option->size < sizeof(OrbisNgs2RackOption)) {
        return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
    }

    outBufferInfo->hostBuffer = nullptr;
    outBufferInfo->hostBufferSize = BlockSize(RackVoices(option));
    MemoryClear(outBufferInfo->reserved, sizeof(outBufferInfo->reserved));
    return ORBIS_OK;
}

s32 RackSetup(OrbisNgs2Handle systemHandle, u32 rackId, const OrbisNgs2RackOption* option,
              const OrbisNgs2ContextBufferInfo* hostBufferInfo, OrbisNgs2BufferFreeHandler hostFree,
              OrbisNgs2Handle* outHandle) {
    Mixer::System* system = SystemFromHandle(systemHandle);
    if (!system) {
        return HandleReportInvalid(systemHandle, 1);
    }
    if (option && option->size < sizeof(OrbisNgs2RackOption)) {
        return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
    }
    if (!hostBufferInfo->hostBuffer) {
        LOG_ERROR(Lib_Ngs2, "Invalid rack buffer address ({})", hostBufferInfo->hostBuffer);
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_ADDRESS;
    }
    const u32 numVoices = RackVoices(option);
    if (hostBufferInfo->hostBufferSize < BlockSize(numVoices)) {
        LOG_ERROR(Lib_Ngs2, "Invalid rack buffer size ({}<{}[byte])",
                  hostBufferInfo->hostBufferSize, BlockSize(numVoices));
        return ORBIS_NGS2_ERROR_INVALID_BUFFER_SIZE;
    }

    Block* block = BlockOf(*hostBufferInfo);
    Mixer::Rack* rack = Mixer::CreateRack(system, rackId, option);
    block->handle = {&block->handle, rack, static_cast<u32>(OrbisNgs2HandleType::Rack), 0};
    block->buffer = *hostBufferInfo;
    block->free = hostFree;
    const OrbisNgs2Handle rackHandle = reinterpret_cast<OrbisNgs2Handle>(block);
    Slot* voices = VoiceSlots(rackHandle);
    for (u32 i = 0; i < numVoices; ++i) {
        voices[i] = {&voices[i], Mixer::GetVoice(rack, i),
                     static_cast<u32>(OrbisNgs2HandleType::Voice), i};
    }
    *outHandle = rackHandle;
    return ORBIS_OK;
}

s32 RackCleanup(OrbisNgs2Handle rackHandle, OrbisNgs2ContextBufferInfo* outInfo) {
    Mixer::Rack* rack = RackFromHandle(rackHandle);
    if (!rack) {
        return HandleReportInvalid(rackHandle, 2);
    }
    Block* block = reinterpret_cast<Block*>(rackHandle);
    OrbisNgs2ContextBufferInfo buffer = block->buffer;
    const OrbisNgs2BufferFreeHandler hostFree = block->free;
    const u32 numVoices = Mixer::NumVoices(rack);
    Mixer::DestroyRack(rack);
    // The handles of the rack and of its voices name nothing from here on.
    MemoryClear(block, sizeof(Block) + numVoices * sizeof(Slot));
    if (hostFree) {
        hostFree(&buffer);
    }
    if (outInfo) {
        *outInfo = buffer;
    }
    return ORBIS_OK;
}

s32 RackVoiceHandle(OrbisNgs2Handle rackHandle, u32 voiceIndex, OrbisNgs2Handle* outHandle) {
    Mixer::Rack* rack = RackFromHandle(rackHandle);
    if (!rack) {
        return HandleReportInvalid(rackHandle, 2);
    }
    if (!outHandle) {
        LOG_ERROR(Lib_Ngs2, "Invalid voice handle address ({})", (void*)outHandle);
        return ORBIS_NGS2_ERROR_INVALID_OUT_ADDRESS;
    }
    if (voiceIndex >= Mixer::NumVoices(rack)) {
        LOG_ERROR(Lib_Ngs2, "Invalid voice index ({})", voiceIndex);
        return ORBIS_NGS2_ERROR_INVALID_VOICE_INDEX;
    }
    *outHandle = reinterpret_cast<OrbisNgs2Handle>(&VoiceSlots(rackHandle)[voiceIndex]);
    return ORBIS_OK;
}

} // namespace Libraries::Ngs2
