// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/recompiled/recompiled.h"

namespace Core::AeroLib {

// Helper to provide stub implementations for missing functions
//
// This works by constructing a minimal trampoline for each new stub which then jumps to a common
// handler with its index provided as a parameter so nid info can be found

struct StubEntry {
    const NidEntry* nid = nullptr;
    std::string nid_unknown;
    std::unique_ptr<Xbyak::CodeGenerator> code;
    // Recompiled mode: the stub, in guest code.
    const u8* guest_code = nullptr;
};

// The stub's address: guest code in recompiled mode, host code otherwise.
static u64 StubAddress(const StubEntry& e) {
    return reinterpret_cast<u64>(e.guest_code != nullptr ? e.guest_code : e.code->getCode());
}

static std::vector<StubEntry> g_stub_entries;
static std::mutex g_stub_mutex;

static u64 PS4_SYSV_ABI CommonStub(u64 index) {
    const auto& e = g_stub_entries[index];
    if (e.nid) {
        LOG_ERROR(Core, "Stub: {} (nid: {}) called, returning zero to {}", e.nid->name, e.nid->nid,
                  __builtin_return_address(0));
    } else {
        LOG_ERROR(Core, "Stub: Unknown (nid: {}) called, returning zero to {}", e.nid_unknown,
                  __builtin_return_address(0));
    }
    return 0;
}

u64 GetStub(const char* nid) {
    std::scoped_lock lock{g_stub_mutex};

    if (g_stub_entries.empty()) {
        g_stub_entries.reserve(500);
    }

    const u64 index = g_stub_entries.size();

    StubEntry e;
    if (const auto* entry = FindByNid(nid)) {
        if (auto const& it = std::ranges::find_if(
                g_stub_entries, [entry](StubEntry const& en) { return en.nid && en.nid == entry; });
            it != g_stub_entries.end()) {
            return StubAddress(*it);
        }
        e.nid = entry;
    } else {
        e.nid_unknown = nid;
    }

    if (Core::Recompiled::Enabled()) {
        // mov rdi, index; mov rax, CommonStub; jmp rax.
        std::array<u8, 22> stub{0x48, 0xbf};
        const u64 handler = reinterpret_cast<u64>(&CommonStub);
        std::memcpy(stub.data() + 2, &index, sizeof(index));
        stub[10] = 0x48;
        stub[11] = 0xb8;
        std::memcpy(stub.data() + 12, &handler, sizeof(handler));
        stub[20] = 0xff;
        stub[21] = 0xe0;
        u8* code = Core::Recompiled::AllocateGuestCode(stub.size());
        std::memcpy(code, stub.data(), stub.size());
        e.guest_code = code;
    } else {
        e.code = std::make_unique<Xbyak::CodeGenerator>(32, Xbyak::AutoGrow);
        e.code->mov(e.code->rdi, index);
        e.code->mov(e.code->rax, reinterpret_cast<u64>(&CommonStub));
        e.code->jmp(e.code->rax);
        e.code->ready();
    }

    g_stub_entries.push_back(std::move(e));
    return StubAddress(g_stub_entries.back());
}

} // namespace Core::AeroLib
