// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/logging/log.h"
#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/hooks.h"
#include "graphics/gr2port/loading_tip.h"
#include "graphics/gr2port/menu_kit.h"

namespace Gr2Port::LoadingTip {

namespace {

using namespace MenuKit;

// void FUN_016ff6d0(icon): draws the loading icon.
constexpr Hooks::Entry IconDraw{0x16ff6d0, 6, Hooks::FramePrologueR14};

// char* FUN_0150bf70(style, key, buffer, size): fills a style from the key's record and returns
// the record's text.
constexpr VAddr FillStyleFunction = 0x150bf70;
// void FUN_0150c110(0, 0, matrix, style, text): queues a text, in pixels of the 1920x1080 canvas.
constexpr VAddr DrawTextFunction = 0x150c110;

// The icon's state, as its draw tests it. Frames is negative while the icon is idle.
constexpr u64 IconPhase = 0x58;
constexpr u64 IconFrames = 0x5c;
constexpr u64 IconQuiet = 0x64;
constexpr u64 IconLoud = 0x70;

constexpr char TipKey[] = "gr2port_tip_loading";
// The game's text beside the icon, whose record gives the tip its style.
constexpr char ModelKey[] = "ui_loading_01";
constexpr char English[] = "TIP: if the display looks blurry, make sure the monitor resolution "
                           "setting in System Menu > Graphics matches your monitor resolution";

// The game's text style, as the icon's draw sets it up.
struct alignas(16) TextStyle {
    u64 magic = 0x73636667;
    s32 alignment = 4;
    float box_width = 300.0f;
    float line = 1.0f;
    std::array<u8, 0x2c> unused{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    float size = 1.2f;
    std::array<float, 3> edge{};
    float edge_opacity = 1.0f;
    s32 edge_kind = 2;
    u32 tail = 0;
};
static_assert(sizeof(TextStyle) == 0x70);

using FillStyle = const char* PS4_SYSV_ABI (*)(TextStyle* style, const char* key, char* buffer,
                                               s32 size);
using DrawText = void PS4_SYSV_ABI (*)(const float* width, const float* height, const float* matrix,
                                       TextStyle* style, const char* text);

std::mutex g_record_mutex;
std::vector<u8> g_record;

// The title menu's answer in each of its two forms: 1 while it is open, then 2 for Continue, 3 for
// Load Game and 4 for New Game. It keeps the answer until the menu opens again.
constexpr std::array<VAddr, 2> TitleAnswers{0x1eee798, 0x1eee820};
constexpr s32 FirstLoadAnswer = 2;

// The tip's centre on the 1920x1080 canvas, level with the icon, and its scale, at which its right
// end stops short of the icon.
constexpr float TipX = 960.0f;
constexpr float TipY = 950.0f;
constexpr float TipScale = 0.85f;
constexpr s32 CentredText = 1;

std::array<s32, TitleAnswers.size()> g_answers{};
// A load from the title menu is due, and its icon has started.
bool g_due = false;
bool g_started = false;

void DrawTip() {
    TextStyle style;
    std::array<char, 0x300> buffer{};
    const char* const text =
        Call<FillStyle>(FillStyleFunction)(&style, TipKey, buffer.data(), buffer.size());
    if (text == buffer.data()) {
        return;
    }
    style.alignment = CentredText;
    style.box_width = 0.0f;
    alignas(16) const std::array<float, 16> matrix{TipScale, 0.0f, 0.0f, 0.0f, 0.0f, TipScale,
                                                   0.0f,     0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                                   TipX,     TipY, 0.0f, 0.0f};
    Call<DrawText>(DrawTextFunction)(nullptr, nullptr, matrix.data(), &style, text);
}

// After each frame's draw of the icon: a new answer from the title menu makes the tip due, and the
// tip stays with the icon's next showing until the icon goes idle.
void PS4_SYSV_ABI OnIconDrawn(u32, const Hooks::Arguments* arguments, u64) {
    for (size_t i = 0; i < TitleAnswers.size(); ++i) {
        const s32 answer = Read<s32>(Game(TitleAnswers[i]));
        g_due |= answer >= FirstLoadAnswer && g_answers[i] < FirstLoadAnswer;
        g_answers[i] = answer;
    }
    if (!g_due) {
        return;
    }
    const VAddr icon = arguments->rdi;
    if (Read<s32>(icon + IconFrames) < 0) {
        g_due = !g_started;
        g_started = false;
        return;
    }
    g_started = true;
    if ((Read<s32>(icon + IconQuiet) == 0 || Read<s32>(icon + IconLoud) != 0) &&
        Read<s32>(icon + IconPhase) > 0) {
        DrawTip();
    }
}

} // namespace

void Install(VAddr module_base, Xbyak::CodeGenerator& c, size_t capacity) {
    if (!Hooks::Hooked(Hooks::ObserveReturn(module_base, IconDraw, c, capacity, &OnIconDrawn, 0))) {
        LOG_ERROR(Render_Vulkan, "GR2port: the loading icon's draw is not the expected code");
    }
}

u64 FindText(u64 texts, const char* key, GraphicsMenu::FindTextFunction find) {
    if (std::strcmp(key, TipKey) != 0) {
        return Core::Recompiled::Call(find, texts, key);
    }
    std::scoped_lock lock{g_record_mutex};
    if (g_record.empty()) {
        const u64 model = Core::Recompiled::Call(find, texts, ModelKey);
        if (model == 0) {
            return 0;
        }
        g_record = MakeRecord(model, Shown(English));
    }
    return reinterpret_cast<u64>(g_record.data());
}

} // namespace Gr2Port::LoadingTip
