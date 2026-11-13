// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/script_timers.h"

namespace Gr2Port::ScriptTimers {

namespace {

constexpr VAddr GoldHenBase = 0x400000;

// The game step's time in seconds, a float.
constexpr VAddr StepTime = 0x1efb104;

// The frames per second the scripts count at.
constexpr float ScriptRate = 30.0f;

// void LoadFunction(LoadState*, Proto*) of Lua 5.2's undump: it fills the function, and calls
// itself for each function inside it.
constexpr Hooks::Entry LoadFunction{0x79b3e0, 6, Hooks::FramePrologue};

// void luaF_freeproto(lua_State*, Proto*).
constexpr Hooks::Entry FreeFunction{0x78c4b0, 6, Hooks::FramePrologueR14};

// A Lua function: its constants, their count, its instruction count and its first and last line.
constexpr u64 Constants = 0x10;
constexpr u64 ConstantCount = 0x54;
constexpr u64 CodeSize = 0x58;
constexpr u64 FirstLine = 0x68;
constexpr u64 LastLine = 0x6c;

// A constant: its value, then its type. A string's characters follow its header, which holds
// the length.
constexpr u64 ConstantBytes = 0x10;
constexpr u64 ConstantType = 0x8;
constexpr u32 NumberType = 3;
constexpr u32 StringType = 4;
constexpr u32 TypeMask = 0xf;
constexpr u64 StringLength = 0x10;
constexpr u64 StringCharacters = 0x18;

// A script function that adds step to a counter once each time it runs: the lines and the
// instruction count that name it in the game's scripts, and a string constant it holds.
struct Timer {
    u32 first_line;
    u32 last_line;
    u32 code_size;
    std::string_view name;
    double step;
};

constexpr std::array Timers{
    // ep04_com_a.lua, _RESET_TIMER_DAT.Check: the count to the next shuffle of the targets in
    // Episode 4's contest with Fi, 1200 calls. The contest's task and Fi's judge report call it
    // once a frame each.
    Timer{1056, 1068, 20, "_RESET_TIMER_DAT", -1.0},
};

// The step constant of a loaded timer function.
struct Loaded {
    u64 function;
    double* constant;
    double step;
};

VAddr g_base = 0;
std::mutex g_mutex;
std::array<Loaded, 8> g_loaded{};

template <typename T>
T Read(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

bool IsString(u64 constant, std::string_view text) {
    if ((Read<u32>(constant + ConstantType) & TypeMask) != StringType) {
        return false;
    }
    const u64 string = Read<u64>(constant);
    return Read<u64>(string + StringLength) == text.size() &&
           std::memcmp(reinterpret_cast<const void*>(string + StringCharacters), text.data(),
                       text.size()) == 0;
}

// The one number constant of the function that equals the timer's step, when the function also
// holds the timer's name.
double* StepConstant(u64 function, const Timer& timer) {
    const u64 constants = Read<u64>(function + Constants);
    const u32 count = Read<u32>(function + ConstantCount);
    double* found = nullptr;
    bool named = false;
    for (u32 i = 0; i < count; i++) {
        const u64 constant = constants + i * ConstantBytes;
        if (IsString(constant, timer.name)) {
            named = true;
        } else if (Read<u32>(constant + ConstantType) == NumberType &&
                   Read<double>(constant) == timer.step) {
            if (found != nullptr) {
                return nullptr;
            }
            found = reinterpret_cast<double*>(constant);
        }
    }
    return named ? found : nullptr;
}

void PS4_SYSV_ABI OnLoaded(u32, const Hooks::Arguments* arguments, u64) {
    const u64 function = arguments->rsi;
    const u32 first_line = Read<u32>(function + FirstLine);
    const u32 last_line = Read<u32>(function + LastLine);
    const u32 code_size = Read<u32>(function + CodeSize);
    for (const Timer& timer : Timers) {
        if (timer.first_line != first_line || timer.last_line != last_line ||
            timer.code_size != code_size) {
            continue;
        }
        double* const constant = StepConstant(function, timer);
        if (constant == nullptr) {
            continue;
        }
        std::scoped_lock lock{g_mutex};
        const auto free = std::ranges::find(g_loaded, u64{0}, &Loaded::function);
        if (free != g_loaded.end()) {
            *free = {function, constant, timer.step};
        }
        return;
    }
}

void PS4_SYSV_ABI OnFree(u32, const Hooks::Registers* registers, u64) {
    std::scoped_lock lock{g_mutex};
    const auto loaded = std::ranges::find(g_loaded, registers->rsi, &Loaded::function);
    if (loaded != g_loaded.end()) {
        *loaded = {};
    }
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    const Hooks::Result loaded =
        Hooks::ObserveReturn(module_base, LoadFunction, c, capacity, &OnLoaded, 0);
    if (loaded != Hooks::Result::Hooked && loaded != Hooks::Result::Chained) {
        LOG_ERROR(Render_Vulkan, "GR2port: the script loader is not the expected code");
        return false;
    }
    const Hooks::Result freed =
        Hooks::ObserveEntry(module_base, FreeFunction, c, capacity, &OnFree, 0);
    if (freed != Hooks::Result::Hooked && freed != Hooks::Result::Chained) {
        LOG_ERROR(Render_Vulkan, "GR2port: the script release is not the expected code");
        return false;
    }
    return true;
}

void OnFrame() {
    if (g_base == 0) {
        return;
    }
    const float step = Read<float>(g_base + StepTime - GoldHenBase);
    const double frames = std::clamp(static_cast<double>(step * ScriptRate), 0.0, 1.0);
    std::scoped_lock lock{g_mutex};
    for (const Loaded& loaded : g_loaded) {
        if (loaded.function != 0) {
            *loaded.constant = loaded.step * frames;
        }
    }
}

} // namespace Gr2Port::ScriptTimers
