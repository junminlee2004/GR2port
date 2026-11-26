// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

#include "core/recompiled/recompiled.h"
#include "graphics/gr2port/ui_items_internal.h"

// The text items: a layout's text boxes and the menu kit's strings. Each draws its glyph mesh, a
// quad of four vertices and six indices per glyph, with the Transform program and the glyph
// program Frame, which samples the font's texture.
namespace Gr2Port::UiItems {

namespace {

// The items' virtual tables. A layout's text box (FUN_013e6be0, built by FUN_013e6090); the menu
// kit's strings (FUN_01510330, built by FUN_0150c110 and FUN_0150d140) and its strings of
// changing characters (FUN_0150ef10), which share one vertex buffer per pool; the items that make
// those buffers (FUN_0150f930), free them (FUN_01510100) and free a string (FUN_01510ec0), which
// draw nothing.
constexpr VAddr LayoutText = 0x1aeb2b8;
constexpr VAddr MenuString = 0x1aedf58;
constexpr VAddr MenuCharacters = 0x1aedf30;
constexpr VAddr MenuBuffersMade = 0x1aedf80;
constexpr VAddr MenuBuffersFreed = 0x1aedfa8;
constexpr VAddr MenuStringFreed = 0x1aedfd0;
// The text of an event script (testTextCreate): FUN_0176e910 adds the item, FUN_0120e280 draws it.
constexpr VAddr ScriptText = 0x1ae4550;

// The vertex shader objects of kind 0 and 1, whose programs at + 0x10 are Transform and
// TransformB (FUN_017b60c0 takes the skinned programs after them only with bones).
constexpr VAddr VertexShaders = 0x1efbee0;
// The glyph program's pixel shader object, slot 3 of the pixel objects. FUN_017b75a0 picks its
// program by a row for the comic values and a column for the pool's screen rectangle; only the
// first, Frame, is ported.
constexpr VAddr GlyphPixelShader = 0x1efbf38;
constexpr VAddr MaskedVertexShader = 0x1efbee8;
constexpr u64 ShaderPrograms = 0x10;
// A vertex program set's inputs, as FUN_01614150 binds them: input u of the count at + 0x28 reads
// the context's buffer of the semantic at + 0x268 + u into the table entry at + 3 of its 16 byte
// record in the records at + 0x58.
constexpr u64 ProgramInputCount = 0x28;
constexpr u64 ProgramInputRecords = 0x58;
constexpr u64 InputRecordBytes = 0x10;
constexpr u64 InputRecordEntry = 3;
constexpr u64 ProgramInputSemantics = 0x268;
constexpr u32 Semantics = 16;

// A vertex buffer object: its attribute count, each attribute's semantic (the last of 4 bytes)
// and V# (16 bytes). The item's function points the context's buffer of each semantic at the V#.
constexpr u64 BufferAttributes = 0x1c;
constexpr u64 AttributeSemantics = 0x37;
constexpr u64 AttributeSharps = 0x64;
// An index buffer object (FUN_01616e10): its buffer, whose first word is the indices' address,
// and its index type, 0 for 16-bit indices.
constexpr u64 IndexType = 0x18;
constexpr u32 IndicesPerGlyph = 6;

// A glyph mesh: the text it is to be built from at + 0, its index buffer object at + 0x8, its
// vertex buffer object at + 0x28 and its glyph count at + 0x198. u32 FUN_01511400(mesh) builds it
// when + 0 holds a text and clears + 0; it gives 2 when the mesh is ready.
constexpr u64 MeshIndices = 0x8;
constexpr u64 MeshVertices = 0x28;
constexpr u64 MeshGlyphs = 0x198;
constexpr VAddr BuildMesh = 0x1511400;
constexpr u32 MeshReady = 2;
using BuildMeshFunction = u32 PS4_SYSV_ABI (*)(VAddr mesh);

// A texture or target object: its size, T# and S#. A font's texture object lies at + 0xa8; the
// engine's default texture object takes its place without a font.
constexpr u64 TextureWidth = 0x14;
constexpr u64 TextureHeight = 0x16;
constexpr u64 TextureSharp = 0x30;
constexpr u64 TextureSampler = 0x50;
constexpr u64 FontTexture = 0xa8;
constexpr VAddr DefaultTexture = 0x1f58600;

// The pool the game fills; the engine's binders take (DAT_01f21bac + 2) % 3 as they run for the
// pool whose screen rectangle they read: its flag, then x, y, width and height, a word each.
constexpr VAddr FilledPool = 0x1f21bac;
constexpr u32 Pools = 3;
constexpr VAddr ScreenRectOn = 0x1f21bb0;
constexpr std::array<VAddr, 4> ScreenRect{0x1f21bb4, 0x1f21bc0, 0x1f21bcc, 0x1f21bd8};
// The comic target's size: width, height, then their reciprocals.
constexpr VAddr ComicTargetSize = 0x1efc5d0;
// Bit 0: the menu kit lays text out at twice the size.
constexpr VAddr DoubleSize = 0x1f2709a;

// A layout's text box item: the address of its glyph mesh's pointer, its 4x4 matrix, its
// material, its comic flag and value, and its alpha.
constexpr u64 TextMeshPointer = 0x18;
constexpr u64 TextMatrix = 0x20;
constexpr u64 TextMaterial = 0x60;
constexpr u64 TextComicFlag = 0x68;
constexpr u64 TextComic = 0x6c;
constexpr u64 TextAlpha = 0x70;
// A material: its vertex shader object, its blend mode (in the high 16 bits from 0x10000 on) and
// its text object (virtual table 0x1aeb3a8 for text): the font at + 0x8, the style's name (a
// string of 0x20 bytes, its characters in place while its capacity is below 16), the style
// (0xffff0000 until looked up), the colour and the outline's width (below 0 until looked up).
constexpr u64 MaterialVertexShader = 0x18;
constexpr u64 MaterialPixelShader = 0x28;
constexpr u64 MaterialBlend = 0x38;
constexpr u64 MaterialText = 0xc0;
constexpr u64 TextFont = 0x8;
constexpr u64 TextStyleName = 0x18;
constexpr u64 TextStyleNameCapacity = 0x30;
constexpr u64 TextStyle = 0x38;
constexpr u64 TextColor = 0x3c;
constexpr u64 TextOutline = 0x4c;
constexpr u32 StyleUnknown = 0xffff0000;
constexpr u32 NoStyle = 0xffffffff;
// The styles FUN_013e7300 draws in white, in black, and with the colour and outline of the
// text object; style 0x27 leaves its colour and outline undefined, NoStyle draws as 0x10.
constexpr u32 StyleWhite = 0;
constexpr u32 StyleBlack = 1;
constexpr u32 StyleUndefined = 0x27;
constexpr u32 StyleOfNone = 0x10;
// long FUN_015165c0(styles, name): the style record of a name, or 0. A record's style is the byte
// at + 0x21 + s + (~s & 3) for the s16 s at + 2, while the s16 at + 4 is not 0; its outline's
// width at + 0xc.
constexpr VAddr FindStyle = 0x15165c0;
constexpr VAddr Styles = 0x1f270a0;
constexpr u64 RecordStyleAt = 0x2;
constexpr u64 RecordHasStyle = 0x4;
constexpr u64 RecordStyle = 0x21;
constexpr u64 RecordOutline = 0xc;
using FindStyleFunction = VAddr PS4_SYSV_ABI (*)(VAddr styles, const char* name);
// FUN_013e7300's comic values: the item's value scaled to 0..2, and the half step around it.
constexpr float ComicScale = 0.007843138f;
constexpr float ComicStep = 0.003921569f;

// A menu kit item: its flags (bit 0: released), its 4x4 matrix, the vertex colour, its alpha
// (drawn while above 0), the pixel colour, the outline's width, the places the game takes the
// text's width and height into (-1 there until measured) and its string block.
constexpr u64 ItemFlags = 0x10;
constexpr u32 ItemReleased = 1;
constexpr u64 MenuMatrix = 0x20;
constexpr u64 MenuVertexColor = 0x60;
constexpr u64 MenuAlpha = 0x6c;
constexpr u64 MenuPixelColor = 0x70;
constexpr u64 MenuOutline = 0x80;
constexpr u64 MenuWidthOut = 0x88;
constexpr u64 MenuHeightOut = 0x90;
constexpr float Unmeasured = -1.0f;
// A script text item: the addresses of its glyph mesh's pointer and its font's pointer, its 4x4
// matrix, its vertex colour, its pixel colour, its outline's width and its blend's factors (colour
// source and destination, alpha source and destination), added.
constexpr u64 ScriptMesh = 0x18;
constexpr u64 ScriptFont = 0x20;
constexpr u64 ScriptMatrix = 0x30;
constexpr u64 ScriptVertexColor = 0x70;
constexpr u64 ScriptPixelColor = 0x80;
constexpr u64 ScriptOutline = 0x90;
constexpr u64 ScriptBlend = 0xb8;
// A string block: its font, glyph mesh and vertex shader object (the engine's kind 0 from the
// first draw on).
constexpr u64 MenuBlock = 0x98;
constexpr u64 BlockFont = 0x8;
constexpr u64 BlockMesh = 0x10;
constexpr u64 BlockVertexShader = 0x18;
constexpr u64 BlockPixelShader = 0x20;
// The blend of the menu kit's text: colour source 1, destination 5, alpha source 0, destination
// 1, added, with separate alpha.
constexpr u32 MenuBlend = 1u << 30 | 1u << 29 | 1u << 24 | 0u << 16 | 5u << 8 | 1u;

// A string of changing characters: its string's index (bit 27 picks the second table), and its
// characters. The strings' tables hold each string's font first. The shared buffers: an index
// buffer object, one vertex buffer object per pool, the glyphs each pool's buffer holds so far,
// the pool those counts are of, the most characters a pool took, the vertex shader object, and
// the flag set once a pool took more than CharacterLimit.
constexpr u64 CharactersString = 0x9c;
constexpr u32 SecondTable = 0x8000000;
constexpr u64 Characters = 0xd8;
constexpr VAddr StringTables = 0x1f26f08;
constexpr VAddr SecondStringTables = 0x1f26f10;
constexpr u32 FontKind = 7;
constexpr u64 FontKindAt = 0x8;
constexpr VAddr CharacterIndices = 0x1f26f00;
constexpr VAddr CharacterVertices = 0x1f27050;
constexpr u64 CharacterVertexBytes = 0x128;
constexpr VAddr CharacterGlyphs = 0x1f27058;
constexpr VAddr CharacterPool = 0x1f27064;
constexpr VAddr CharacterMost = 0x1f27068;
constexpr VAddr CharacterVertexShader = 0x1f27070;
constexpr VAddr CharacterPixelShader = 0x1f27078;
constexpr VAddr CharactersFull = 0x1f27099;
constexpr u32 CharacterLimit = 0x280;

u32 BinderPool() {
    return (Load<u32>(Game(FilledPool)) + 2) % Pools;
}

// A font's texture object.
VAddr TextureOf(VAddr font) {
    return font != 0 ? font + FontTexture : Game(DefaultTexture);
}

// The draw's inputs for the vertex program set program from the vertex buffer object buffer.
void BindInputs(VAddr program, VAddr buffer, Draw& draw) {
    std::array<VAddr, Semantics> sharps{};
    const u32 attributes = Load<u16>(buffer + BufferAttributes);
    for (u32 i = 0; i < attributes; i++) {
        const u32 semantic = Load<u8>(buffer + AttributeSemantics + i * 4);
        if (semantic < Semantics) {
            sharps[semantic] = buffer + AttributeSharps + i * 16;
        }
    }
    const u32 inputs = Load<u8>(program + ProgramInputCount);
    const VAddr records = Load<VAddr>(program + ProgramInputRecords);
    for (u32 i = 0; i < inputs; i++) {
        const u32 semantic = Load<u8>(program + ProgramInputSemantics + i);
        const u32 entry = Load<u8>(records + i * InputRecordBytes + InputRecordEntry);
        if (semantic >= Semantics || sharps[semantic] == 0 || entry >= draw.inputs.size()) {
            continue;
        }
        const auto words = Load<std::array<u32, 4>>(sharps[semantic]);
        draw.inputs[entry] = {
            .address = u64{words[0]} | u64{words[1] & 0xfff} << 32,
            .stride = words[1] >> 16 & 0x3fff,
            .format = words[3],
            .elements = words[2],
        };
    }
}

// FUN_017b59b0's constants of the Transform program: the 4x4 matrix, the colour, and the texture
// coordinates' transform, the identity (1, 0, 0.5, 0.5) for text.
void TransformConstants(VAddr matrix, const std::array<float, 4>& color, Draw& draw) {
    std::array<float, 24> constants;
    std::memcpy(constants.data(), reinterpret_cast<const void*>(matrix), 16 * sizeof(float));
    std::ranges::copy(color, constants.begin() + 16);
    std::ranges::copy(std::array{1.0f, 0.0f, 0.5f, 0.5f}, constants.begin() + 20);
    draw.vs_bytes = sizeof(constants);
    draw.vs_constants = Keep(constants.data(), draw.vs_bytes);
}

// FUN_017b7740's constants of the glyph program: the colour, the outline's width and height in
// the font texture's units and two more words, the comic target's reciprocal size, 0, 0 and the
// comic's range when there is a comic, then the pool's screen rectangle while one is set.
void GlyphConstants(const std::array<float, 4>& color, const std::array<float, 4>& outline,
                    const std::array<float, 4>* comic, Draw& draw) {
    std::array<u32, 20> constants{};
    u32 words = 0;
    const auto add = [&](float value) { constants[words++] = std::bit_cast<u32>(value); };
    std::ranges::for_each(color, add);
    std::ranges::for_each(outline, add);
    if (comic != nullptr) {
        add(Load<float>(Game(ComicTargetSize) + 8));
        add(Load<float>(Game(ComicTargetSize) + 12));
        add(0.0f);
        add(0.0f);
        std::ranges::for_each(*comic, add);
    }
    const u32 pool = BinderPool();
    if (Load<u8>(Game(ScreenRectOn) + pool) != 0) {
        for (const VAddr side : ScreenRect) {
            constants[words++] = Load<u32>(Game(side) + pool * sizeof(u32));
        }
    }
    draw.ps_bytes = words * sizeof(u32);
    draw.ps_constants = Keep(constants.data(), draw.ps_bytes);
}

// A draw of glyphs from the vertex buffer object vertices and the index buffer object indices
// with the vertex shader object shader and the pixel shader object pixel, sampling the texture
// object texture. Comic values and the screen rectangle pick programs that are not ported, and so
// do other pixel objects (the masked text's).
Draw GlyphDraw(VAddr shader, VAddr pixel, bool comic, VAddr vertices, VAddr indices,
               VAddr texture) {
    Draw draw{};
    draw.vertex = shader == Load<VAddr>(Game(MaskedVertexShader))
                      ? Scene::UiVertexProgram::TransformB
                      : Scene::UiVertexProgram::Transform;
    const bool rect = Load<u8>(Game(ScreenRectOn) + BinderPool()) != 0;
    draw.pixel = pixel == Load<VAddr>(Game(GlyphPixelShader)) && !comic && !rect
                     ? Scene::UiPixelProgram::Frame
                     : Scene::UiPixelProgram::Count;
    draw.target = Target();
    draw.viewport = Viewport(draw.target);
    draw.topology = Scene::UiPrimitiveList;
    BindInputs(shader + ShaderPrograms, vertices, draw);
    draw.indices = Load<VAddr>(Load<VAddr>(indices));
    draw.index_bits = Load<u32>(indices + IndexType) != 0 ? 32 : 16;
    draw.textures[0] = {.sharp = texture + TextureSharp, .sampler = texture + TextureSampler};
    draw.flags = Scene::UiDrawText | Scene::UiDrawAnchored;
    return draw;
}

u32 BuildGlyphs(VAddr mesh) {
    return Core::Recompiled::Call(reinterpret_cast<BuildMeshFunction>(Game(BuildMesh)), mesh);
}

// The style record of a text object's style name.
VAddr StyleRecord(VAddr text) {
    const VAddr name = Load<u64>(text + TextStyleNameCapacity) >= 16
                           ? Load<VAddr>(text + TextStyleName)
                           : text + TextStyleName;
    return Core::Recompiled::Call(reinterpret_cast<FindStyleFunction>(Game(FindStyle)),
                                  Load<VAddr>(Game(Styles)), reinterpret_cast<const char*>(name));
}

// FUN_013e6be0: builds its glyph mesh when its text changed, then draws it through FUN_013e7300
// with its material, whose text object gives the pixel colour and outline by its style.
void ReadLayoutText(VAddr item) {
    const VAddr mesh = Load<VAddr>(Load<VAddr>(item + TextMeshPointer));
    BuildGlyphs(mesh);
    const VAddr material = Load<VAddr>(item + TextMaterial);
    const VAddr text = Load<VAddr>(material + MaterialText);
    const VAddr texture = TextureOf(Load<VAddr>(text + TextFont));
    const float alpha = Load<float>(item + TextAlpha);

    u32 style = Load<u32>(text + TextStyle);
    if (style == StyleUnknown) {
        const VAddr record = StyleRecord(text);
        style = NoStyle;
        if (record != 0 && Load<s16>(record + RecordHasStyle) != 0) {
            const s16 at = Load<s16>(record + RecordStyleAt);
            style = Load<u8>(record + at + RecordStyle + (~at & 3));
        }
    }
    if (style == NoStyle) {
        style = StyleOfNone;
    }
    std::array<float, 4> color{};
    std::array<float, 4> outline{};
    if (style == StyleWhite) {
        color = {1.0f, 1.0f, 1.0f, alpha};
    } else if (style == StyleBlack) {
        color = {0.0f, 0.0f, 0.0f, alpha};
    } else if (style != StyleUndefined) {
        color = Load<std::array<float, 4>>(text + TextColor);
        float width = Load<float>(text + TextOutline);
        if (width < 0.0f) {
            const VAddr record = StyleRecord(text);
            width = record != 0 ? Load<float>(record + RecordOutline) : 0.0f;
        }
        outline[0] = width / static_cast<float>(Load<u16>(texture + TextureWidth));
        outline[1] = width / static_cast<float>(Load<u16>(texture + TextureHeight));
    }
    // The outline's last two words, and with a comic the range's last two, are bytes of
    // FUN_013e7300's stack it never writes; they stay 0 here.
    std::array<float, 4> comic{};
    const float comic_value = Load<float>(item + TextComic);
    if (comic_value != 0.0f) {
        const float scaled = comic_value * ComicScale;
        if (Load<u8>(item + TextComicFlag) == 0) {
            comic = {scaled - ComicStep, scaled + ComicStep, 0.0f, 0.0f};
        } else {
            comic = {scaled, 0.0f, 0.0f, 0.0f};
        }
    }

    Draw draw = GlyphDraw(Load<VAddr>(material + MaterialVertexShader),
                          Load<VAddr>(material + MaterialPixelShader), comic_value != 0.0f,
                          mesh + MeshVertices, mesh + MeshIndices, texture);
    const u32 mode = Load<u32>(material + MaterialBlend);
    draw.blend = Blend(mode < 0x10000 ? static_cast<s32>(mode) : static_cast<s32>(mode) >> 16);
    draw.first = 0;
    draw.count = Load<u32>(mesh + MeshGlyphs) * IndicesPerGlyph;
    TransformConstants(item + TextMatrix, {1.0f, 1.0f, 1.0f, alpha}, draw);
    GlyphConstants(color, outline, comic_value != 0.0f ? &comic : nullptr, draw);
    Add(draw);
}

// The menu kit's constants and draw state of an item drawing with the font texture texture, its
// outline width scaled by scale.
void MenuConstants(VAddr item, VAddr texture, float scale, Draw& draw) {
    const float width = scale * Load<float>(item + MenuOutline);
    const std::array<float, 4> outline{
        width / static_cast<float>(Load<u16>(texture + TextureWidth)),
        width / static_cast<float>(Load<u16>(texture + TextureHeight)), 0.0f, 0.0f};
    draw.blend = MenuBlend;
    TransformConstants(item + MenuMatrix, Load<std::array<float, 4>>(item + MenuVertexColor), draw);
    GlyphConstants(Load<std::array<float, 4>>(item + MenuPixelColor), outline, nullptr, draw);
}

bool Measures(VAddr out) {
    return out != 0 && Load<float>(out) == Unmeasured;
}

// FUN_01510330: on its first run builds its glyph mesh when its text changed and draws it while
// its alpha is above 0, unless it measures the text this run; on its second run it releases the
// string.
void ReadMenuString(VAddr item) {
    if ((Load<u8>(item + ItemFlags) & ItemReleased) != 0) {
        return;
    }
    const VAddr block = Load<VAddr>(item + MenuBlock);
    const VAddr mesh = Load<VAddr>(block + BlockMesh);
    if (BuildGlyphs(mesh) != MeshReady || !(Load<float>(item + MenuAlpha) > 0.0f) ||
        Measures(Load<VAddr>(item + MenuWidthOut)) || Measures(Load<VAddr>(item + MenuHeightOut))) {
        return;
    }
    // The first draw sets the block's shader objects to the kind 0 vertex object and the glyph
    // program's pixel object.
    VAddr shader = Load<VAddr>(block + BlockVertexShader);
    VAddr pixel = Load<VAddr>(block + BlockPixelShader);
    if (shader == 0) {
        shader = Load<VAddr>(Game(VertexShaders));
        pixel = Load<VAddr>(Game(GlyphPixelShader));
    }
    const VAddr texture = TextureOf(Load<VAddr>(block + BlockFont));
    Draw draw = GlyphDraw(shader, pixel, false, mesh + MeshVertices, mesh + MeshIndices, texture);
    draw.first = 0;
    draw.count = Load<u32>(mesh + MeshGlyphs) * IndicesPerGlyph;
    MenuConstants(item, texture, (Load<u8>(Game(DoubleSize)) & 1) != 0 ? 2.0f : 1.0f, draw);
    Add(draw);
}

// FUN_0150ef10: writes a glyph per printable character into the pool's shared vertex buffer
// after the glyphs already there, and draws them while its alpha is above 0 and it measures
// nothing.
void ReadMenuCharacters(VAddr item) {
    if ((Load<u8>(Game(CharactersFull)) & 1) != 0) {
        return;
    }
    const u32 index = Load<u32>(item + CharactersString);
    const VAddr strings =
        Load<VAddr>(Game((index & SecondTable) != 0 ? SecondStringTables : StringTables));
    const VAddr string = Load<VAddr>(strings + u64{index & (SecondTable - 1)} * sizeof(VAddr));
    if (string == 0) {
        return;
    }
    const VAddr font = Load<VAddr>(string);
    if (font == 0 || Load<u32>(font + FontKindAt) != FontKind) {
        return;
    }
    const u32 pool = BinderPool();
    const u32 first = pool == Load<u32>(Game(CharacterPool))
                          ? Load<u32>(Game(CharacterGlyphs) + pool * sizeof(u32))
                          : 0;
    const char* const characters = reinterpret_cast<const char*>(item + Characters);
    const auto length = static_cast<u32>(std::strlen(characters));
    if (std::max(Load<u32>(Game(CharacterMost)), first + length) > CharacterLimit ||
        Load<VAddr>(item + MenuWidthOut) != 0 || Load<VAddr>(item + MenuHeightOut) != 0 ||
        !(Load<float>(item + MenuAlpha) > 0.0f)) {
        return;
    }
    u32 glyphs = 0;
    for (u32 i = 0; i < length; i++) {
        if (static_cast<s8>(characters[i]) > 0x1f) {
            glyphs++;
        }
    }
    const VAddr texture = TextureOf(font);
    Draw draw =
        GlyphDraw(Load<VAddr>(Game(CharacterVertexShader)), Load<VAddr>(Game(CharacterPixelShader)),
                  false, Load<VAddr>(Game(CharacterVertices)) + u64{pool} * CharacterVertexBytes,
                  Load<VAddr>(Game(CharacterIndices)), texture);
    draw.first = first * IndicesPerGlyph;
    draw.count = glyphs * IndicesPerGlyph;
    MenuConstants(item, texture, 1.0f, draw);
    Add(draw);
}

// FUN_0120e280: the glyph mesh of *(+ 0x18) in the font of *(+ 0x20), with the kind 0 vertex
// object's matrix at + 0x30 and colour at + 0x70, the glyph program's colour at + 0x80 and outline
// width at + 0x90, blending by the factors at + 0xb8.
void ReadScriptText(VAddr item) {
    const VAddr mesh = Load<VAddr>(Load<VAddr>(item + ScriptMesh));
    const VAddr font = Load<VAddr>(Load<VAddr>(item + ScriptFont));
    const VAddr texture = TextureOf(font);
    Draw draw = GlyphDraw(Load<VAddr>(Game(VertexShaders)), Load<VAddr>(Game(GlyphPixelShader)),
                          false, mesh + MeshVertices, mesh + MeshIndices, texture);
    const auto factors = Load<std::array<u32, 4>>(item + ScriptBlend);
    draw.blend =
        factors[0] | factors[1] << 8 | factors[2] << 16 | factors[3] << 24 | 1u << 29 | 1u << 30;
    draw.first = 0;
    draw.count = Load<u32>(mesh + MeshGlyphs) * IndicesPerGlyph;
    const float width = Load<float>(item + ScriptOutline);
    const std::array<float, 4> outline{
        width / static_cast<float>(Load<u16>(texture + TextureWidth)),
        width / static_cast<float>(Load<u16>(texture + TextureHeight)), 0.0f, 0.0f};
    TransformConstants(item + ScriptMatrix, Load<std::array<float, 4>>(item + ScriptVertexColor),
                       draw);
    GlyphConstants(Load<std::array<float, 4>>(item + ScriptPixelColor), outline, nullptr, draw);
    Add(draw);
}

void ReadNothing(VAddr) {}

} // namespace

std::span<const Kind> TextKinds() {
    static const std::array<Kind, 7> kinds{{
        {Game(LayoutText), &ReadLayoutText},
        {Game(ScriptText), &ReadScriptText},
        {Game(MenuString), &ReadMenuString},
        {Game(MenuCharacters), &ReadMenuCharacters},
        {Game(MenuBuffersMade), &ReadNothing},
        {Game(MenuBuffersFreed), &ReadNothing},
        {Game(MenuStringFreed), &ReadNothing},
    }};
    return kinds;
}

} // namespace Gr2Port::UiItems
