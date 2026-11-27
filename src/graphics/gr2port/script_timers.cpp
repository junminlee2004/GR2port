// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/script_timers.h"
#include "graphics/gr2port/settings.h"

namespace Gr2Port::ScriptTimers {

namespace {

// void LoadFunction(LoadState*, Proto*) of Lua 5.2's undump: it fills the function, and calls
// itself for each function inside it.
constexpr Hooks::Entry LoadFunction{0x79b3e0, 6, Hooks::FramePrologue};

// void luaF_freeproto(lua_State*, Proto*).
constexpr Hooks::Entry FreeFunction{0x78c4b0, 6, Hooks::FramePrologueR14};

// A Lua function: its constants, their count, its instruction count and its first and last line.
constexpr u64 Constants = 0x10;
constexpr u64 Code = 0x18;
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
    // le_rainbow.lua, LeRainbow._setCountTime's task: subtracts 1 once a frame from the time the
    // rainbow stays up after it fades in, 18000 calls.
    Timer{233, 245, 19, "time", 1.0},
    // ep06_e.lua, the task of the damage to Lei Elgona while Kat puts out the fires in Episode 6:
    // once a frame it adds a rate to des_point for each fire still burning, and the gauge shows
    // des_point out of 100. The rate is one of four, by how many enemies are left.
    Timer{708, 777, 111, "des_point", 0.001},
    Timer{708, 777, 111, "des_point", 0.0015},
    Timer{708, 777, 111, "des_point", 0.002},
    Timer{708, 777, 111, "des_point", 0.003},
};

// The step constant of a loaded timer function.
struct Loaded {
    u64 function;
    double* constant;
    double step;
};

// A count of 30 fps frames in a script function that runs every frame: the function's first and
// last line and instruction count, and the index of the instruction that steps the count. The rest
// of the function keeps running every frame, which a hold renewed each frame or a value of one
// frame needs.
struct FrameCount {
    u32 first_line;
    u32 last_line;
    u32 code_size;
    u32 pc;
};

constexpr std::array FrameCounts{
    // ep11_com_a.lua: holds the camera on a target for a number of seconds.
    FrameCount{1938, 1945, 13, 8},
    // sm91.lua: two holds of the camera on a target, 60 frames each.
    FrameCount{109, 116, 18, 16},
    FrameCount{149, 154, 14, 12},
    // ep90_a.lua, ep90_header.lua and ep91_a.lua: how long the camera stick has moved while the
    // camera holds a target, which lets go of it after 10 frames.
    FrameCount{239, 282, 84, 42},
    FrameCount{914, 966, 120, 61},
    FrameCount{173, 225, 120, 61},
    // sm01_b.lua, sm01_c.lua and sm01_d.lua: the 300 frames to the enemies' next attack.
    FrameCount{125, 801, 1086, 515},
    FrameCount{206, 1291, 1792, 367},
    FrameCount{118, 472, 557, 369},
    // cm11.lua and sm07_com_a.lua: the caution meter that rises while Kat is in a bomb's sensor and
    // falls while she is out of it, by a speed a frame.
    FrameCount{970, 1160, 184, 31},
    FrameCount{970, 1160, 184, 54},
    FrameCount{677, 865, 155, 51},
    FrameCount{677, 865, 155, 72},
    // sm40_a.lua: the 20 frames out of range before the dog stops following.
    FrameCount{694, 730, 62, 42},
    // sm41_b.lua: the frames before the markers on the last boxes appear.
    FrameCount{156, 457, 516, 294},
    // sm44_c2.lua: the frames Kat stays near the father, up to FOLLOW_CLEAR_TIME.
    FrameCount{148, 456, 472, 212},
    // ep12_u.lua: the 600 frames Kat can stay in two or more of a moving wall's four sensors before
    // the walls crush her, and the 5 frames out of them that start the 600 again.
    FrameCount{984, 1029, 99, 66},
    FrameCount{984, 1029, 99, 77},
    // ep10_e.lua: the 30 frames between the shots of a cannon that shells Kat, each a tenth of her
    // life.
    FrameCount{1398, 1511, 155, 146},
    // sm19_b.lua: the 300 frames between the cries Kat hears within CRYING_DISTANCE, each with a
    // sound, a caption and a turn of the camera.
    FrameCount{131, 1002, 1107, 632},
    // ep07_j.lua: the frames between the timed captions of a fight, and the 900 between its hints.
    FrameCount{197, 310, 155, 77},
    FrameCount{197, 310, 155, 85},
    // sm40_c.lua: the seconds, 30 frames each, before a photo hint shows and between its repeats
    // while Kat is in the hint's sensor.
    FrameCount{1040, 1079, 51, 38},
};

// A script function whose passes each stand for a 30 fps frame, where a held count would not do:
// the function's first and last line and instruction count. Its waits last until a frame that
// completes a 30 fps frame.
struct WholeFrames {
    u32 first_line;
    u32 last_line;
    u32 code_size;
};

constexpr std::array WholeFrameFunctions{
    // sm33_c.lua: the frames Kat stays 15 m or more from the girl with her out of sight. At 150 a
    // caption warns Kat and at 300 the mission starts over, each when the count equals the number,
    // which a held count would repeat on every frame it holds.
    WholeFrames{219, 277, 90},
    // dm04_f.lua: while Kat stands on a damaging floor, each pass takes a 30th of FLOOR_DAMAGE_RATE
    // times her full life.
    WholeFrames{529, 562, 43},
};

// cage_controller.lua, Cage._move: each pass of its loop moves the cage one 30 fps frame's step
// with setTransform, whose call returns to the first instruction, and ends in a wait, whose call
// returns to the second.
constexpr u32 CageFirstLine = 1261;
constexpr u32 CageLastLine = 1459;
constexpr u32 CageCodeSize = 339;
constexpr u64 CageStepSet = 117;
constexpr u64 CageStepEnd = 148;

// The native setTransform, whose first argument is a game object's userdata.
constexpr Hooks::Entry SetTransform{0x16c9e20, 6, Hooks::FramePrologue};
constexpr u64 FirstArgument = 0x10;
constexpr u64 UserdataObject = 0x30;

// A game object's local position, and its flags with the bit for a changed transform.
constexpr u64 LocalPosition = 0x2a0;
constexpr u64 TransformFlags = 0x328;
constexpr u8 TransformChanged = 1;

// Lua 5.2 instructions: the opcode, the operands B and C with their flag of a constant, and the
// jump offset.
constexpr u32 OpcodeMask = 0x3f;
constexpr u32 OpMove = 0;
constexpr u32 OpAdd = 13;
constexpr u32 OpSub = 14;
constexpr u32 OpJump = 23;
constexpr u32 OpForLoop = 32;
constexpr u32 OperandBShift = 23;
constexpr u32 OperandCShift = 14;
constexpr u32 OperandCMask = 0x1ff;
constexpr u32 OperandConstant = 0x100;
constexpr u32 JumpOffsetMask = 0xffffc000;

// lua_State: its current call. A call: the function, the call before it, its status with the bit
// of a Lua function, and a Lua function's next instruction. A Lua closure: its function.
constexpr u64 StateCall = 0x20;
constexpr u64 CallPrevious = 0x10;
constexpr u64 CallStatus = 0x22;
constexpr u8 CallIsLua = 1;
constexpr u64 CallNextInstruction = 0x38;
constexpr u64 ClosureFunction = 0x18;

// _waitSandbox2, the native under the script function wait: xor esi, esi; xor edx, edx;
// lea rcx, [continuation]; jmp lua_yieldk. These bytes are the frame rate patches'.
constexpr Hooks::Entry WaitNative{
    0x15d4350, 11, {0x31, 0xf6, 0x31, 0xd2, 0x48, 0x8d, 0x0d, 0x35, 0x32, 0xec, 0xfe}};
constexpr VAddr WaitContinuation = 0x497590;
constexpr VAddr LuaYield = 0x78b430;
constexpr size_t WaitStubBytes = 112;

// The frame rate patches' flag of a frame that completes a 30 fps frame, how far the frame is into
// the next 30 fps frame from 0 to 1, and the game's count of frames. The patches of more than
// 30 fps also keep the frame's length in 30 fps frames, at most 1, which leaves out the game's
// slow motion.
constexpr VAddr FrameShare = 0x1f7c600;
constexpr VAddr WholeFrame = 0x1f7c608;
constexpr VAddr FramePhase = 0x1f7c604;
constexpr VAddr GameFrames = 0x1f302e4;

// A loaded function with a frame count, a cage's steps or whole frames: the instruction that steps
// the count, as the script has it and with the count held. The instruction is null for the other
// two.
struct Stepped {
    std::atomic<u64> function;
    u32* instruction;
    u32 counting;
    u32 held;
    bool whole_frames;
};

using Position = std::array<float, 4>;

// A cage on its way, by the coroutine that moves it: the game object and its vtable, the positions
// before and after its last step, the game frame of that step, and whether the object is between
// the two.
struct CageRide {
    u64 coroutine;
    VAddr object;
    VAddr type;
    Position from;
    Position to;
    u32 frame;
    bool moving;
};

VAddr g_base = 0;
std::mutex g_mutex;
std::array<Loaded, 8> g_loaded{};
bool g_counts_hooked = false;
bool g_frame_share = false;
std::atomic<u32> g_stepped_count{0};
std::array<Stepped, 16> g_stepped{};
std::array<CageRide, 8> g_rides{};

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

bool IsNumber(u64 constant, double value) {
    return Read<u32>(constant + ConstantType) == NumberType && Read<double>(constant) == value;
}

// The instruction that leaves a count where it is: a for loop's jump back without its step, an
// addition of 1 made an addition of the function's constant 0, or an addition of a register made
// a copy of the count. Returns 0 for any other.
u32 HeldInstruction(u64 function, u32 instruction) {
    const u32 opcode = instruction & OpcodeMask;
    if (opcode == OpForLoop) {
        return OpJump | (instruction & JumpOffsetMask);
    }
    if (opcode != OpAdd && opcode != OpSub) {
        return 0;
    }
    const u32 operand = (instruction >> OperandCShift) & OperandCMask;
    if ((operand & OperandConstant) == 0) {
        if (((instruction >> OperandBShift) & OperandConstant) != 0) {
            return 0;
        }
        return OpMove | (instruction & ~(OpcodeMask | (OperandCMask << OperandCShift)));
    }
    const u64 constants = Read<u64>(function + Constants);
    const u32 count = std::min(Read<u32>(function + ConstantCount), OperandConstant);
    const u32 one = operand & ~OperandConstant;
    if (one >= count || !IsNumber(constants + one * ConstantBytes, 1.0)) {
        return 0;
    }
    for (u32 i = 0; i < count; i++) {
        if (IsNumber(constants + i * ConstantBytes, 0.0)) {
            return (instruction & ~(OperandCMask << OperandCShift)) |
                   ((OperandConstant | i) << OperandCShift);
        }
    }
    return 0;
}

void AddStepped(u64 function, u32* instruction, u32 counting, u32 held, bool whole_frames) {
    std::scoped_lock lock{g_mutex};
    for (Stepped& stepped : g_stepped) {
        if (stepped.function.load(std::memory_order_relaxed) == 0) {
            stepped.instruction = instruction;
            stepped.counting = counting;
            stepped.held = held;
            stepped.whole_frames = whole_frames;
            stepped.function.store(function, std::memory_order_relaxed);
            g_stepped_count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void AddFrameCount(u64 function, u32 first_line, u32 last_line, u32 code_size) {
    if (first_line == CageFirstLine && last_line == CageLastLine && code_size == CageCodeSize) {
        AddStepped(function, nullptr, 0, 0, false);
        return;
    }
    for (const WholeFrames& whole : WholeFrameFunctions) {
        if (whole.first_line == first_line && whole.last_line == last_line &&
            whole.code_size == code_size) {
            AddStepped(function, nullptr, 0, 0, true);
            return;
        }
    }
    for (const FrameCount& count : FrameCounts) {
        if (count.first_line != first_line || count.last_line != last_line ||
            count.code_size != code_size) {
            continue;
        }
        u32* const code = Read<u32*>(function + Code);
        const u32 held = HeldInstruction(function, code[count.pc]);
        if (held != 0) {
            AddStepped(function, code + count.pc, code[count.pc], held, false);
        }
    }
}

void PS4_SYSV_ABI OnLoaded(u32, const Hooks::Arguments* arguments, u64) {
    const u64 function = arguments->rsi;
    const u32 first_line = Read<u32>(function + FirstLine);
    const u32 last_line = Read<u32>(function + LastLine);
    const u32 code_size = Read<u32>(function + CodeSize);
    if (g_counts_hooked) {
        AddFrameCount(function, first_line, last_line, code_size);
    }
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
    }
}

void PS4_SYSV_ABI OnFree(u32, const Hooks::Registers* registers, u64) {
    std::scoped_lock lock{g_mutex};
    const auto loaded = std::ranges::find(g_loaded, registers->rsi, &Loaded::function);
    if (loaded != g_loaded.end()) {
        *loaded = {};
    }
    for (Stepped& stepped : g_stepped) {
        if (stepped.function.load(std::memory_order_relaxed) == registers->rsi) {
            stepped.function.store(0, std::memory_order_relaxed);
            g_stepped_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }
}

u32 GameFrame() {
    return Read<u32>(g_base + GameFrames - Hooks::CodeBase);
}

bool IsWholeFrame() {
    return (Read<u8>(g_base + WholeFrame - Hooks::CodeBase) & 1) != 0;
}

u64 NextInstruction(u64 call, u64 function) {
    return (Read<u64>(call + CallNextInstruction) - Read<u64>(function + Code)) / sizeof(u32);
}

void WritePosition(VAddr object, const Position& position) {
    std::memcpy(reinterpret_cast<void*>(object + LocalPosition), position.data(), sizeof(position));
    *reinterpret_cast<u8*>(object + TransformFlags) |= TransformChanged;
}

// Before a cage's step: notes the object and the position the step starts from.
void PS4_SYSV_ABI OnSetTransform(u32, const Hooks::Registers* registers, u64) {
    if (g_stepped_count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const u64 state = registers->rdi;
    const u64 native = Read<u64>(state + StateCall);
    const u64 caller = Read<u64>(native + CallPrevious);
    if (caller == 0 || (Read<u8>(caller + CallStatus) & CallIsLua) == 0) {
        return;
    }
    const u64 function = Read<u64>(Read<u64>(Read<u64>(caller)) + ClosureFunction);
    const bool is_cage = std::ranges::any_of(g_stepped, [function](const Stepped& stepped) {
        return stepped.instruction == nullptr && !stepped.whole_frames &&
               stepped.function.load(std::memory_order_relaxed) == function;
    });
    if (!is_cage || NextInstruction(caller, function) != CageStepSet) {
        return;
    }
    auto ride = std::ranges::find(g_rides, state, &CageRide::coroutine);
    if (ride == g_rides.end()) {
        ride = std::ranges::find(g_rides, false, &CageRide::moving);
    }
    if (ride == g_rides.end()) {
        return;
    }
    const u64 userdata = Read<u64>(Read<u64>(native) + FirstArgument);
    const VAddr object = Read<VAddr>(userdata + UserdataObject);
    *ride = {.coroutine = state,
             .object = object,
             .type = Read<VAddr>(object),
             .from = Read<Position>(object + LocalPosition)};
}

// A cage's script resumes on a whole frame: its next step starts from the position of its last.
void PS4_SYSV_ABI OnCageResume(u64 state) {
    const auto ride = std::ranges::find(g_rides, state, &CageRide::coroutine);
    if (ride != g_rides.end() && ride->moving) {
        WritePosition(ride->object, ride->to);
        ride->moving = false;
    }
}

// Runs at each call of the wait native. Returns 1 when the script waits for a whole frame.
u32 PS4_SYSV_ABI OnWait(u64 state) {
    if (g_stepped_count.load(std::memory_order_relaxed) == 0) {
        return 0;
    }
    const u64 wait = Read<u64>(Read<u64>(state + StateCall) + CallPrevious);
    if (wait == 0) {
        return 0;
    }
    const u64 caller = Read<u64>(wait + CallPrevious);
    if (caller == 0 || (Read<u8>(caller + CallStatus) & CallIsLua) == 0) {
        return 0;
    }
    const u64 function = Read<u64>(Read<u64>(Read<u64>(caller)) + ClosureFunction);
    for (Stepped& stepped : g_stepped) {
        if (stepped.function.load(std::memory_order_relaxed) != function) {
            continue;
        }
        std::scoped_lock lock{g_mutex};
        if (stepped.function.load(std::memory_order_relaxed) != function) {
            return 0;
        }
        if (stepped.whole_frames) {
            return 1;
        }
        if (stepped.instruction == nullptr) {
            if (NextInstruction(caller, function) != CageStepEnd) {
                return 0;
            }
            const auto ride = std::ranges::find(g_rides, state, &CageRide::coroutine);
            if (ride != g_rides.end()) {
                ride->to = Read<Position>(ride->object + LocalPosition);
                ride->frame = GameFrame();
                ride->moving = true;
            }
            return 1;
        }
        *stepped.instruction = IsWholeFrame() ? stepped.counting : stepped.held;
    }
    return 0;
}

} // namespace

bool Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    g_base = module_base;
    // At the game's own 30 fps the timers keep their stock steps.
    g_frame_share = Settings::Get().game.frame_rate > 30;
    const Hooks::Result loaded =
        Hooks::ObserveReturn(module_base, LoadFunction, c, capacity, &OnLoaded, 0);
    if (!Hooks::Hooked(loaded)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the script loader is not the expected code");
        return false;
    }
    const Hooks::Result freed =
        Hooks::ObserveEntry(module_base, FreeFunction, c, capacity, &OnFree, 0);
    if (!Hooks::Hooked(freed)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the script release is not the expected code");
        return false;
    }
    return true;
}

bool InstallFrameCounts(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    using namespace Xbyak::util;
    if (!Hooks::Holds(module_base, WaitNative)) {
        LOG_ERROR(Render_Vulkan, "GR2port: the scripts' wait is not the expected code");
        return false;
    }
    if (capacity - c.getSize() < WaitStubBytes + 2 * Hooks::MaxStubBytes) {
        LOG_ERROR(Render_Vulkan, "GR2port: no room for the scripts' wait hook");
        return false;
    }
    const auto code = [module_base](VAddr address) { return Hooks::Code(module_base, address); };

    // A cage's wait, and each wait of a function that runs on whole frames, yields to its own
    // continuation, which yields again until a whole frame and then returns through the game's
    // continuation.
    const void* stub = c.getCurr();
    Xbyak::Label hold;
    Xbyak::Label resume;
    c.push(rdi);
    c.mov(rax, reinterpret_cast<u64>(&OnWait));
    c.call(rax);
    c.pop(rdi);
    c.test(eax, eax);
    c.jnz(hold);
    c.xor_(esi, esi);
    c.xor_(edx, edx);
    c.lea(rcx, ptr[rip + code(WaitContinuation)]);
    c.jmp(code(LuaYield));
    c.L(hold);
    c.xor_(esi, esi);
    c.xor_(edx, edx);
    c.lea(rcx, ptr[rip + resume]);
    c.jmp(code(LuaYield));
    c.L(resume);
    c.test(byte[rip + code(WholeFrame)], 1);
    c.jz(hold);
    c.push(rdi);
    c.mov(rax, reinterpret_cast<u64>(&OnCageResume));
    c.call(rax);
    c.pop(rdi);
    c.jmp(code(WaitContinuation));
    if (Hooks::Replace(module_base, WaitNative, c, capacity, stub) != Hooks::Result::Hooked) {
        LOG_ERROR(Render_Vulkan, "GR2port: the scripts' wait hook did not go in");
        return false;
    }
    if (!Hooks::Hooked(
            Hooks::ObserveEntry(module_base, SetTransform, c, capacity, &OnSetTransform, 0))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the scripts' setTransform is not the expected code");
        return false;
    }
    g_counts_hooked = true;
    return true;
}

void PlaceCages() {
    if (!g_counts_hooked) {
        return;
    }
    const bool whole = IsWholeFrame();
    const u32 frame = GameFrame();
    const float share = Read<float>(g_base + FramePhase - Hooks::CodeBase);
    for (CageRide& ride : g_rides) {
        if (!ride.moving) {
            continue;
        }
        if (Read<VAddr>(ride.object) != ride.type) {
            ride = {};
            continue;
        }
        // A whole frame resumes every running ride, so this one's script has stopped.
        if (whole && ride.frame != frame) {
            WritePosition(ride.object, ride.to);
            ride.moving = false;
            continue;
        }
        Position position = ride.to;
        for (size_t i = 0; i < 3; i++) {
            position[i] = ride.from[i] + (ride.to[i] - ride.from[i]) * share;
        }
        WritePosition(ride.object, position);
    }
}

void OnFrame() {
    if (g_base == 0 || !g_frame_share) {
        return;
    }
    const double frames = Read<float>(g_base + FrameShare - Hooks::CodeBase);
    std::scoped_lock lock{g_mutex};
    for (const Loaded& loaded : g_loaded) {
        if (loaded.function != 0) {
            *loaded.constant = loaded.step * frames;
        }
    }
}

} // namespace Gr2Port::ScriptTimers
