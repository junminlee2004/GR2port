// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <immintrin.h>

#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/ui_items_internal.h"

// The screen drawing's full-screen passes: the HUD glow's fill, threshold, blurs and composite,
// the fade, the movie, the photo, ScreenDraw's captured background (FUN_0150aba0) and its colour
// control.
namespace Gr2Port::UiItems {

namespace {

using Scene::UiPixelProgram;
using Scene::UiVertexProgram;

// The draw thread's engine context, TLS[-0x38]: the one of index DAT_01f585f0. Its blend: on,
// the colour and alpha operations, then the colour source and destination and the alpha source
// and destination factors.
constexpr VAddr Contexts = 0x1f315d8;
constexpr u64 ContextBytes = 0xcff8;
constexpr VAddr ContextIndex = 0x1f585f0;
constexpr u64 ContextBlendOn = 0xcc08;
constexpr u64 ContextBlendOps = 0xcc0c;
constexpr u64 ContextBlendFactors = 0xcc14;

// The pool the game fills, the frame's back buffer object, the whole-screen target and its flag
// per pool, which makes it the screen drawing's target in place of the back buffer.
constexpr VAddr ItemPool = 0x1f21bac;
constexpr u32 ItemPoolCount = 3;
constexpr VAddr BackBuffer = 0x1f315a8;
constexpr VAddr ScreenTarget = 0x1f26358;
constexpr VAddr ScreenTargetFlags = 0x1f26c90;

// A target or texture object: its memory, size, T#, a word whose sign bit is set while the
// binders may bind it, and S#.
constexpr u64 ObjectMemory = 0x8;
constexpr u64 ObjectWidth = 0x14;
constexpr u64 ObjectHeight = 0x16;
constexpr u64 ObjectSharp = 0x30;
constexpr u64 ObjectState = 0x3c;
constexpr u64 ObjectSampler = 0x50;
constexpr u64 TargetBytes = 0x118;

// The glow's targets by id (FUN_013ef010): 1 the full-size layer, 3 and 2 (or 4) the two small
// ones, 5 and 6 the pair after them, 5 the one DAT_01efc5e0 picks.
constexpr VAddr GlowLayer = 0x1efc058;
constexpr VAddr GlowSmall = 0x1efc170;
constexpr VAddr GlowSmallB = 0x1efc288;
constexpr VAddr GlowParity = 0x1efc5e0;

// ScreenDraw's targets: the conversion of 1920x1620 movie frames (planes stacked), the movie
// frame of other sizes, made at the frame's size on first use, and the copy the colour control
// reads.
constexpr VAddr StackedTarget = 0x1f25d18;
constexpr VAddr MovieTarget = 0x1f26d48;
constexpr VAddr ColorTarget = 0x1f26b60;
constexpr u16 StackedWidth = 1920;
constexpr u16 StackedHeight = 1620;

// Engine vertex objects (FUN_0161bc80's layout): the attribute count at + 0x1c and a V# per
// attribute from + 0x64, in the order of the vertex program's inputs. Each pass's full-screen strip
// of four vertices, a position, a texture coordinate and a colour, built by FUN_013eedc0: the
// fill's and the other passes'. ScreenDraw's own strip, built by FUN_00854a50.
constexpr u64 VertexCount = 0x1c;
constexpr u64 VertexSharps = 0x64;
constexpr VAddr FillVertices = 0x1efbc88;
constexpr VAddr PassVertices = 0x1efbdb0;
constexpr VAddr MasterVertices = 0x1f26230;
constexpr u32 QuadVertices = 4;

// The blur's taps and the flag FUN_013edee0 sets on its first run; until then it blurs without
// the history.
constexpr u32 BlurTaps = 16;
constexpr VAddr BlurReady = 0x1efbc82;
// expf, the game's import FUN_013edee0 calls, and the sigma it takes for 0.
constexpr VAddr ExpImport = 0x17f6980;
constexpr u32 SmallestSigma = 0x3727c5ac;
// The history's last constant, 1/255.
constexpr u32 HistoryStep = 0x3b808081;

// The colour control's constants, which DAT_01efbf88's binder FUN_017b7d50 binds.
constexpr VAddr ColorControlConstants = 0x1efbfc0;
constexpr u32 ColorControlBytes = 0x90;

// The photo manager: the target each shot goes into and its vertex object. The filters, one
// object each: the filter program's constants at + 0x10, which their virtual + 0x28
// (FUN_01398da0) gives. The captured frame, the picture while DAT_01f26cb8 is not 0. The film
// frame's texture object.
constexpr VAddr PhotoManager = 0x1eed278;
constexpr u64 PhotoTarget = 0x580;
constexpr u64 PhotoVertices = 0x458;
constexpr VAddr FilterObjects = 0x1ee31d0;
constexpr u64 FilterBytes = 0x2020;
constexpr u64 FilterCurves = 0x10;
constexpr u32 CurveBytes = 0x2000;
constexpr VAddr CapturedFrame = 0x1f26c80;
constexpr VAddr CapturedHeld = 0x1f26cb8;
constexpr VAddr FilmFrame = 0x1ee31c8;
constexpr u64 FilmTexture = 0xb0;

// The vertex program's constants every Master draw and the shot bind: its colour. The fade's
// colour, which it binds in their place.
constexpr std::array<float, 4> MasterColor{1.0f, 1.0f, 1.0f, 1.0f};
constexpr u64 FadeColor = 0x20;
constexpr u32 FadeColorBytes = 16;

// The item kinds of this file, by virtual table.
constexpr VAddr FillItem = 0x1aeb4a8;
constexpr VAddr ThresholdItem = 0x1aeb4d0;
constexpr VAddr BlurItem = 0x1aeb4f8;
constexpr VAddr SwitchItem = 0x1aeb480;
constexpr VAddr CompositeItem = 0x1aeb520;
constexpr VAddr FlipItem = 0x1aeb548;
constexpr VAddr FadeItem = 0x1aedee0;
constexpr VAddr MovieItem = 0x1aedeb8;
constexpr VAddr BackgroundItem = 0x1aede90;
constexpr VAddr PhotoItem = 0x1ae2ba0;
// Layout steps, async callbacks, the screen rectangle's setters and an object's release
// (FUN_00c56bb0), which draw nothing.
constexpr std::array<VAddr, 13> SilentItems{
    0x1aeb2e0, 0x1aeb308, 0x1aeb330, 0x1aeb358, 0x1afe748, 0x1afef98, 0x1aff140,
    0x1aedf08, 0x1ae2028, 0x1aff8f8, 0x1aede40, 0x1aede68, 0x1ac2358,
};

// The screen drawing's target: the back buffer, or the whole-screen target while the flag of the
// pool the walk runs is set.
VAddr Screen() {
    const u32 pool = (Load<u32>(Game(ItemPool)) + 2) % ItemPoolCount;
    return Load<u8>(Game(ScreenTargetFlags) + pool) != 0 ? Game(ScreenTarget)
                                                         : Load<VAddr>(Game(BackBuffer));
}

// A glow target id's object, as the passes' switches resolve it: id 0 is the screen drawing's
// target; an id without a target gives 0.
VAddr PassTarget(s32 id) {
    switch (id) {
    case 0:
        return Screen();
    case 1:
        return Game(GlowLayer);
    case 2:
    case 4:
        return Game(GlowSmallB);
    case 3:
        return Game(GlowSmall);
    case 5:
    case 6: {
        const u32 parity = Load<u32>(Game(GlowParity)) ^ (id == 6 ? 1u : 0u);
        return Game(GlowSmall) + (parity + 2) * TargetBytes;
    }
    default:
        return 0;
    }
}

// The target FUN_013ef010 sets for an id: the screen drawing's for an id without one.
VAddr SetPassTarget(s32 id) {
    const VAddr target = PassTarget(id);
    return target != 0 ? target : Screen();
}

// An object's texture as the binders bind it (FUN_017b8040): only while it has memory and its
// state word's sign bit is set.
Texture Bound(VAddr object) {
    if (object == 0 || Load<u64>(object + ObjectMemory) == 0 ||
        Load<s32>(object + ObjectState) >= 0) {
        return {};
    }
    return {object + ObjectSharp, object + ObjectSampler};
}

// CB_BLEND0_CONTROL as FUN_01614150 writes it for a blend that is on: separate alpha, always.
u32 BlendControl(const std::array<u32, 2>& ops, const std::array<u32, 4>& factors) {
    return (factors[0] & 0x1f) | (ops[0] & 7) << 5 | (factors[1] & 0x1f) << 8 |
           (factors[2] & 0x1f) << 16 | (ops[1] & 7) << 21 | (factors[3] & 0x1f) << 24 | 1u << 29 |
           1u << 30;
}

// The engine context's blend now, as its next draw writes it; 0 while blending is off.
u32 ContextBlend() {
    const VAddr context = Game(Contexts) + Load<u32>(Game(ContextIndex)) * ContextBytes;
    if (Load<u8>(context + ContextBlendOn) == 0) {
        return 0;
    }
    return BlendControl(Load<std::array<u32, 2>>(context + ContextBlendOps),
                        Load<std::array<u32, 4>>(context + ContextBlendFactors));
}

// The inputs an engine vertex object gives, from its V#s.
void SetInputs(Draw& draw, VAddr vertices) {
    const u32 count = Load<u16>(vertices + VertexCount);
    for (u32 i = 0; i < count; i++) {
        const auto words = Load<std::array<u32, 4>>(vertices + VertexSharps + u64{i} * 16);
        draw.inputs[i] = {
            .address = words[0] | u64{words[1] & 0xfff} << 32,
            .stride = words[1] >> 16 & 0x3fff,
            .format = words[3],
            .elements = words[2],
        };
    }
}

// A full-screen strip of four vertices into target (FUN_01617340(6, 4)), with the inputs of an
// engine vertex object, or none for 0.
Draw Quad(UiVertexProgram vertex, UiPixelProgram pixel, VAddr target, u32 blend, VAddr vertices) {
    Draw draw{};
    draw.vertex = vertex;
    draw.pixel = pixel;
    draw.target = target;
    draw.blend = blend;
    draw.topology = Scene::UiPrimitiveStrip;
    draw.viewport = Viewport(target);
    draw.count = QuadVertices;
    if (vertices != 0) {
        SetInputs(draw, vertices);
    }
    return draw;
}

template <typename T, size_t Count>
void SetVertexConstants(Draw& draw, const std::array<T, Count>& constants) {
    draw.vs_bytes = static_cast<u32>(sizeof(constants));
    draw.vs_constants = Keep(constants.data(), draw.vs_bytes);
}

template <typename T, size_t Count>
void SetPixelConstants(Draw& draw, const std::array<T, Count>& constants) {
    draw.ps_bytes = static_cast<u32>(sizeof(constants));
    draw.ps_constants = Keep(constants.data(), draw.ps_bytes);
}

// A Master + Copy of source into target, as FUN_0150aba0, FUN_0150b6e0 and FUN_0084b700 draw it.
void AddCopy(VAddr source, VAddr target, u32 blend) {
    Draw draw =
        Quad(UiVertexProgram::Master, UiPixelProgram::Copy, target, blend, Game(MasterVertices));
    SetVertexConstants(draw, MasterColor);
    draw.textures[0] = Bound(source);
    Add(draw);
}

// A PassFlat + Movie draw of a movie frame into target, with the frame's size as constants.
Draw Conversion(VAddr frame, VAddr target, u32 blend) {
    Draw draw = Quad(UiVertexProgram::PassFlat, UiPixelProgram::Movie, target, blend, 0);
    SetPixelConstants(draw, std::array<float, 4>{
                                static_cast<float>(Load<u16>(frame + ObjectWidth)),
                                static_cast<float>(Load<u16>(frame + ObjectHeight)),
                                0.0f,
                                0.0f,
                            });
    draw.textures[0] = Bound(frame);
    return draw;
}

// 1 / x as the game computes it: rcpps, then one Newton step. rcpss gives each lane's rcpps.
float Reciprocal(float x) {
    const float guess = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
    const float error = 1.0f - x * guess;
    return guess + guess * error;
}

// FUN_013edee0's tap weights: exp(-k * k / 2 / sigma) for k = 0 to 15, over their sum with each
// counted twice, in its order of operations.
std::array<float, BlurTaps> BlurWeights(float sigma) {
    using Exp = float PS4_SYSV_ABI (*)(float);
    const auto exponential = reinterpret_cast<Exp>(Game(ExpImport));
    const float inverse = Reciprocal(sigma == 0.0f ? std::bit_cast<float>(SmallestSigma) : sigma);
    std::array<float, BlurTaps> weights;
    for (u32 k = 0; k < BlurTaps; k++) {
        weights[k] =
            Core::Recompiled::Call(exponential, inverse * (-0.5f * static_cast<float>(k * k)));
    }
    constexpr u32 Half = BlurTaps / 2;
    std::array<float, Half> pairs;
    for (u32 k = 0; k < Half; k++) {
        pairs[k] = (weights[Half + k] + weights[Half + k]) + (weights[k] + weights[k]);
    }
    std::array<float, 4> sums;
    for (u32 k = 0; k < 4; k++) {
        sums[k] = pairs[k] + pairs[k + 4];
    }
    const float scale = Reciprocal((sums[0] + sums[2]) + (sums[1] + sums[3]));
    for (float& weight : weights) {
        weight = scale * weight;
    }
    return weights;
}

// FUN_013edee0(sigma, weight, source, target, horizontal, history): Pass + Blur of source into
// target along one axis, or Pass + BlurHistory, which mixes in the history target by weight once
// the pass has run before. Blending off; target stays set.
void AddBlur(float sigma, float weight, VAddr source, VAddr target, bool horizontal,
             VAddr history) {
    const bool mixes = history != 0 && weight != 0.0f && (Load<u8>(Game(BlurReady)) & 1) != 0;
    const auto weights = BlurWeights(sigma);
    const float texel =
        1.0f / static_cast<float>(Load<u16>(source + (horizontal ? ObjectWidth : ObjectHeight)));
    const float span = texel * 16.0f;
    const std::array<float, 4> step =
        horizontal ? std::array{span, 0.0f, texel, 0.0f} : std::array{0.0f, span, 0.0f, texel};
    SetTarget(target);
    Draw draw =
        Quad(UiVertexProgram::Pass, mixes ? UiPixelProgram::BlurHistory : UiPixelProgram::Blur,
             target, 0, Game(PassVertices));
    std::array<float, BlurTaps + 8> constants{};
    std::copy(weights.begin(), weights.end(), constants.begin());
    std::copy(step.begin(), step.end(), constants.begin() + BlurTaps);
    if (mixes) {
        constants[BlurTaps + 4] = 1.0f - weight;
        constants[BlurTaps + 5] = weight;
        constants[BlurTaps + 6] = std::bit_cast<float>(HistoryStep);
        SetPixelConstants(draw, constants);
        draw.textures[1] = Bound(history);
    } else {
        draw.ps_bytes = static_cast<u32>((BlurTaps + 4) * sizeof(float));
        draw.ps_constants = Keep(constants.data(), draw.ps_bytes);
    }
    draw.textures[0] = Bound(source);
    Add(draw);
}

// FUN_013ed950: Pass + Color of a black, clear strip into the target of + 0x14, blending
// ONE/ZERO. Read after the function, which makes the passes' strips on its first run.
void ReadFill(VAddr item) {
    const VAddr target = SetPassTarget(Load<s32>(item + 0x14));
    SetTarget(target);
    Add(Quad(UiVertexProgram::Pass, UiPixelProgram::Color, target,
             BlendControl({0, 0}, {1, 0, 1, 0}), Game(FillVertices)));
}

// FUN_013edb80: Pass + Threshold of the target of + 0x18 into that of + 0x14, with + 0x1c and
// + 0x20 as constants. Blending off. Read after the function, as the fill.
void ReadThreshold(VAddr item) {
    const VAddr target = SetPassTarget(Load<s32>(item + 0x14));
    SetTarget(target);
    Draw draw =
        Quad(UiVertexProgram::Pass, UiPixelProgram::Threshold, target, 0, Game(PassVertices));
    SetPixelConstants(draw, std::array<u32, 4>{Load<u32>(item + 0x1c), Load<u32>(item + 0x20)});
    draw.textures[0] = Bound(PassTarget(Load<s32>(item + 0x18)));
    Add(draw);
}

// FUN_013ee590: a blur of the target of + 0x18 into that of + 0x14, sigma at + 0x24, horizontal
// when + 0x28 is set, mixing in the target of + 0x1c by the weight at + 0x20.
void ReadBlur(VAddr item) {
    AddBlur(Load<float>(item + 0x24), Load<float>(item + 0x20), PassTarget(Load<s32>(item + 0x18)),
            PassTarget(Load<s32>(item + 0x14)), Load<u8>(item + 0x28) != 0,
            PassTarget(Load<s32>(item + 0x1c)));
}

// FUN_013ed890: sets the target of + 0x14.
void ReadSwitch(VAddr item) {
    SetTarget(SetPassTarget(Load<s32>(item + 0x14)));
}

// FUN_013ee790: Pass + Sprite of the target of + 0x14 over the current target, the intensity at
// + 0x18 as constants, blending by the mode at + 0x1c. A negative mode draws through program
// 1bb9a1cf, which GR2port does not have, into target 1 and copies that over the screen drawing's
// target; neither draw is read.
void ReadComposite(VAddr item) {
    const s32 mode = Load<s32>(item + 0x1c);
    if (mode < 0) {
        SetTarget(Screen());
        return;
    }
    Draw draw = Quad(UiVertexProgram::Pass, UiPixelProgram::Sprite, Target(), Blend(mode),
                     Game(PassVertices));
    SetPixelConstants(draw, std::array<u32, 4>{Load<u32>(item + 0x18)});
    draw.textures[0] = Bound(PassTarget(Load<s32>(item + 0x14)));
    Add(draw);
}

// FUN_0150b4c0: Master + Gamma over the current target, the colour at + 0x20 as the vertex
// program's constants, premultiplied.
void ReadFade(VAddr item) {
    Draw draw = Quad(UiVertexProgram::Master, UiPixelProgram::Gamma, Target(), Blend(1),
                     Game(MasterVertices));
    draw.vs_constants = item + FadeColor;
    draw.vs_bytes = FadeColorBytes;
    Add(draw);
}

// FUN_0150b6e0: the movie frame at + 0x18, larger than 1x1, converted into StackedTarget (a
// 1920x1620 frame) or MovieTarget, then copied over the screen drawing's target; otherwise the
// frame copied over the current target. Both with the current blend.
void ReadMovie(VAddr item) {
    const VAddr frame = Load<VAddr>(item + 0x18);
    const u32 blend = ContextBlend();
    const u16 width = frame != 0 ? Load<u16>(frame + ObjectWidth) : 0;
    const u16 height = frame != 0 ? Load<u16>(frame + ObjectHeight) : 0;
    if (width <= 1 || height <= 1) {
        AddCopy(frame, Target(), blend);
        return;
    }
    const bool stacked = width == StackedWidth && height == StackedHeight;
    const VAddr target = Game(stacked ? StackedTarget : MovieTarget);
    Draw draw = Conversion(frame, target, blend);
    if (!stacked && Load<u64>(target + ObjectMemory) == 0) {
        // FUN_01618370 makes it at the frame's size.
        draw.viewport = {0.5f * width, 0.5f * width, -0.5f * height, 0.5f * height};
    }
    SetTarget(target);
    Add(draw);
    const VAddr screen = Screen();
    SetTarget(screen);
    AddCopy(target, screen, blend);
}

// FUN_011afe90: Photo + Photo of the picture and the film frame into the photo manager's target,
// with the filter of + 0x20. Blending off; it sets the screen drawing's target again after.
void ReadPhoto(VAddr item) {
    const VAddr manager = Load<VAddr>(Game(PhotoManager));
    const VAddr target = manager + PhotoTarget;
    SetTarget(target);
    Draw draw =
        Quad(UiVertexProgram::Photo, UiPixelProgram::Photo, target, 0, manager + PhotoVertices);
    SetVertexConstants(draw, MasterColor);
    draw.ps_constants = Game(FilterObjects) + Load<u32>(item + 0x20) * FilterBytes + FilterCurves;
    draw.ps_bytes = CurveBytes;
    const VAddr picture =
        Load<u32>(Game(CapturedHeld)) == 0 ? Game(ScreenTarget) : Load<VAddr>(Game(CapturedFrame));
    draw.textures[0] = Bound(picture);
    draw.textures[1] = Bound(Load<VAddr>(Game(FilmFrame)) + FilmTexture);
    Add(draw);
    SetTarget(Screen());
}

// Items that draw nothing and leave the target: the history flip (FUN_013eed20), the captured
// background (FUN_0150b6c0, read at FUN_0150aba0's entry) and the silent items.
void ReadNothing(VAddr) {}

} // namespace

std::span<const Kind> PassKinds() {
    static const auto kinds = [] {
        std::array<Kind, 10 + SilentItems.size()> list{{
            {Game(FillItem), &ReadNothing, &ReadFill},
            {Game(ThresholdItem), &ReadNothing, &ReadThreshold},
            {Game(BlurItem), &ReadBlur},
            {Game(SwitchItem), &ReadSwitch},
            {Game(CompositeItem), &ReadComposite},
            {Game(FlipItem), &ReadNothing},
            {Game(FadeItem), &ReadFade},
            {Game(MovieItem), &ReadMovie},
            {Game(BackgroundItem), &ReadNothing},
            {Game(PhotoItem), &ReadPhoto},
        }};
        for (size_t i = 0; i < SilentItems.size(); i++) {
            list[10 + i] = {Game(SilentItems[i]), &ReadNothing};
        }
        return list;
    }();
    return kinds;
}

// FUN_0150aba0(blur, source, mode). Mode 2 converts a 1920x1620 source into StackedTarget and
// copies StackedTarget. A blur above 0 copies a source of another size than GlowSmallB into
// GlowSmall, then blurs across into GlowSmallB and down into GlowSmall, and copies that. The
// copy goes over the screen drawing's target. FUN_0084b700 passes mode 0 only while its back
// buffer is the target: before the walk with DAT_01f26c90 clear, and after 0x84d45c sets it.
void Background(float blur, VAddr source, u32 mode) {
    u32 blend = ContextBlend();
    if (mode == 2) {
        const VAddr stacked = Game(StackedTarget);
        if (Load<u16>(source + ObjectWidth) == StackedWidth &&
            Load<u16>(source + ObjectHeight) == StackedHeight &&
            Load<u64>(stacked + ObjectMemory) != 0) {
            SetTarget(stacked);
            Add(Conversion(source, stacked, blend));
        }
        SetTarget(Screen());
        source = stacked;
    } else if (!(blur > 0.0f)) {
        SetTarget(Load<VAddr>(Game(BackBuffer)));
    }
    if (blur > 0.0f) {
        const VAddr across = Game(GlowSmallB);
        const VAddr down = Game(GlowSmall);
        if (Load<u16>(source + ObjectWidth) != Load<u16>(across + ObjectWidth) ||
            Load<u16>(source + ObjectHeight) != Load<u16>(across + ObjectHeight)) {
            SetTarget(down);
            AddCopy(source, down, blend);
            source = down;
        }
        AddBlur(blur, 0.0f, source, across, true, 0);
        AddBlur(blur, 0.0f, across, down, false, 0);
        blend = 0;
        source = down;
        SetTarget(Screen());
    }
    AddCopy(source, Target(), blend);
}

// FUN_0084b700's colour control: Master + Copy of the back buffer into ColorTarget, then Master +
// ColorControl of ColorTarget over the back buffer, both with the blend the walk left.
void ColorControl() {
    const u32 blend = ContextBlend();
    const VAddr back = Load<VAddr>(Game(BackBuffer));
    const VAddr copy = Game(ColorTarget);
    SetTarget(copy);
    AddCopy(back, copy, blend);
    SetTarget(back);
    Draw draw = Quad(UiVertexProgram::Master, UiPixelProgram::ColorControl, back, blend,
                     Game(MasterVertices));
    SetVertexConstants(draw, MasterColor);
    draw.ps_bytes = ColorControlBytes;
    draw.ps_constants =
        Keep(reinterpret_cast<const void*>(Game(ColorControlConstants)), ColorControlBytes);
    draw.textures[0] = Bound(copy);
    Add(draw);
}

} // namespace Gr2Port::UiItems
