// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <string_view>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/settings.h"
#include "graphics/gr2port/smoke_and_dust.h"
#include "graphics/gr2port/smoke_effects.h"

namespace Gr2Port::SmokeAndDust {

namespace {

constexpr u32 NameHash(std::string_view name) {
    u32 hash = 0x811c9dc5;
    for (const char c : name) {
        hash = (hash ^ static_cast<u8>(c)) * 0x01000193;
    }
    return hash;
}

// The empty name's hash, for which the spawn makes no effect.
constexpr u32 NoEffect = NameHash("");

constexpr auto SmokeHashes = [] {
    std::array<u32, SmokeEffects::Files.size()> hashes{};
    std::ranges::transform(SmokeEffects::Files, hashes.begin(), NameHash);
    std::ranges::sort(hashes);
    return hashes;
}();

// u64* FUN_014a6990(u64* out, library, u32 hash): writes to out an effect of the loaded file whose
// name has hash, or 0.
constexpr Hooks::Entry Spawn{0x14a6990, 6, Hooks::FramePrologue};
using SpawnFunction = u64* PS4_SYSV_ABI (*)(u64* out, VAddr library, u32 hash);

// The spawn's return address in FUN_00d68070, which loads an effect into the effect registry.
constexpr VAddr RegistryLoad = 0xd68136;

SpawnFunction g_spawn = nullptr;
VAddr g_registry_load = 0;

u64* PS4_SYSV_ABI SpawnEffect(u64* out, VAddr library, u32 hash) {
    const VAddr caller = Core::Recompiled::GuestCaller(__builtin_return_address(0));
    const bool refused = caller != g_registry_load && std::ranges::binary_search(SmokeHashes, hash);
    return Core::Recompiled::Call(g_spawn, out, library, refused ? NoEffect : hash);
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    if (Settings::Get().graphics.smoke_and_dust) {
        return true;
    }
    const void* original = nullptr;
    if (!Hooks::Hooked(Hooks::Replace(module_base, Spawn, c, capacity,
                                      reinterpret_cast<const void*>(&SpawnEffect), &original))) {
        LOG_ERROR(Render_Vulkan,
                  "GR2port: the effect spawn is not the expected code, so smoke and dust show");
        return false;
    }
    g_spawn = reinterpret_cast<SpawnFunction>(const_cast<void*>(original));
    g_registry_load = module_base + RegistryLoad - Hooks::CodeBase;
    LOG_INFO(Render_Vulkan, "GR2port: smoke and dust are off: {} effect files make no effect",
             SmokeHashes.size());
    return true;
}

} // namespace Gr2Port::SmokeAndDust
