// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "video_core/gr2port/capture_math.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/particle_capture.h"
#include "video_core/gr2port/particles.h"
#include "video_core/gr2port/textures.h"

namespace Gr2Port::ParticleCapture {

namespace {

using Capture::Invert;
using Capture::Load;
using Capture::Matrix;
using Capture::Multiply;

// The particle manager: its renderer and the curves waiting to go into the atlas.
constexpr u64 FxRenderer = 0x8;
constexpr u64 FxCurves = 0x100;
constexpr u64 FxCurvesEnd = 0x108;
// A curve waiting: a reference to a loaded texture, or the texture FUN_014a3cb0 made and filled;
// the first description it is for, and its columns.
constexpr u64 CurveTexture = 0x8;
constexpr u64 CurveDestination = 0x10;
constexpr u64 CurveColumns = 0x14;
// A texture FUN_014a3cb0 made: its bytes, of which a row of texels takes 1 << TextureRowShift, and
// the block whose first word points at its texels.
constexpr u64 TextureBytes = 0x24;
constexpr u32 TextureRowShift = 9;
constexpr u64 TextureMemory = 0x28;
// A texture reference: its object when the kind says it is a loaded texture.
constexpr u64 ReferenceKind = 0x20;
constexpr u32 ReferenceLoaded = 4;
constexpr u64 ReferenceObject = 0xb0;
constexpr u64 ObjectOfReference = 8;

// The manager's effects with models, and an effect's emitter; such a model's entry names its
// object, its kind and how many particles precede it. Effects with these flags draw no model.
constexpr u64 FxModelEffects = 0xa8;
constexpr u64 FxModelEffectsEnd = 0xb0;
constexpr u64 ModelEffectFlags = 0x4a;
constexpr u8 ModelEffectSkippedFlags = 0x18;
constexpr u64 ModelEffectEmitter = 0x20;
constexpr u64 EmitterObjectBase = 0x6c;
constexpr u64 ModelEntryObject = 0x8;
constexpr u64 ModelEntryOff = 0x10;
constexpr u64 ModelEntryKind = 0x14;
constexpr u32 ModelEntryKindPlaced = 3;
// The view's list of meshes that place particles on themselves, filled while the game builds its
// scene: world matrix by rows, vertex buffer object, object, skin entry or -1, emitter object base,
// how many it precedes, and kind.
constexpr u64 ViewSamples = 0xbf0;
constexpr u64 ViewSampleCount = 0xbf8;
constexpr u64 ViewSampleSkins = 0xbd8;
constexpr u64 SampleBytes = 0x50;
constexpr u64 SampleVertexBuffer = 0x30;
constexpr u64 SampleObject = 0x38;
constexpr u64 SampleSkin = 0x40;
constexpr u64 SampleEmitter = 0x44;
constexpr u64 SampleEmitterOffset = 0x48;
constexpr u64 SampleKind = 0x49;
// A skin entry of the view: the skin stream's vertex buffer object, the palette and its bones.
constexpr u64 SkinEntryBytes = 0x20;
constexpr u64 SkinEntryPalette = 0x8;
constexpr u64 SkinEntryBones = 0x10;
constexpr u64 BoneBytes = 0x30;
// What of a vertex buffer object the sampling reads: its stride, vertices, the element of each
// semantic, and each element's offset, as FUN_014b3860 does.
constexpr u64 VertexBufferStride = 0x18;
constexpr u64 VertexBufferVertices = 0x20;
constexpr u64 VertexBufferSemantics = 0x24;
constexpr u64 VertexBufferOffsets = 0x36;
constexpr u32 VertexBufferNoElement = 0x10;
constexpr u32 SemanticPosition = 0;
constexpr u32 SemanticWeights = 1;
constexpr u32 SemanticNormal = 2;
constexpr u32 SemanticBones = 7;
constexpr u64 ShapeVertexBufferObject = 0x60;
constexpr u64 ObjectIdentity = 0xb8;

// The renderer: the frame's items, the block of camera values and the engine's manager.
constexpr u64 RendererTable = 0x10;
constexpr u64 RendererContext = 0x18;
constexpr u64 RendererManager = 0x40;
constexpr u64 TableItems = 0x8;
constexpr u64 TableCount = 0x20;
constexpr u64 ItemFirst = 0x20;
constexpr u64 ItemCount = 0x24;
constexpr u64 ItemResource = 0x50;
constexpr u64 ItemEmitter = 0x90;
constexpr u64 ItemKind = 0xb4;
constexpr u64 ItemMode = 0xb8;
constexpr u64 ItemModelKey = 0x98;
constexpr u32 KindSprite = 1;
constexpr u32 KindRibbon = 2;
constexpr u32 KindModel = 3;
constexpr u32 KindMesh = 4;
constexpr u32 ShadowPass = 3;
constexpr u32 SpriteVertices = 4;
constexpr u32 RibbonVertices = 10;
constexpr u64 ResourceMaterial = 0x20;
constexpr u64 ResourceKey = 0x28;
constexpr u64 EmitterState = 0x74;
constexpr u32 EmitterStateMask = 0xf;
constexpr u32 EmitterStateHidden = 2;
constexpr u64 EmitterSystem = 0x78;
constexpr u64 SystemAttribute = 0x60;
// The system's models: entries that name an item's key and a resource's, each with a set of
// objects.
constexpr u64 SystemModels = 0x8;
constexpr u64 SystemModelsEnd = 0x10;
constexpr u64 ModelEntryKey = 0x18;
constexpr u64 ModelEntryResource = 0x1c;
constexpr u64 ModelObjects = 0x10;
constexpr u64 ModelObjectsEnd = 0x18;
// The colours a mesh particle's height blends between, on its object or else its shape.
constexpr u64 ObjectGradationHigh = 0x2e0;
constexpr u64 ObjectGradationLow = 0x2f0;
constexpr u64 ShapeGradationHigh = 0x40;
constexpr u64 ShapeGradationLow = 0x50;
constexpr float GradationNone = 0.5f;
constexpr float GradationLeast = 1e-6f;
// An object's shape, and what of a shape FUN_014bd080 reads: only shapes with vertex colours and
// tangents draw, and each submesh draws its triangles.
constexpr u64 ObjectShapes = 0xb0;
constexpr u64 ObjectShapeOffset = 0xbc;
constexpr u64 ShapeSubmeshCount = 0x10;
constexpr u64 ShapeElements = 0x7a;
constexpr u16 ModelElements = 0x4008;
constexpr u64 ShapeSubmeshes = 0x1e0;
constexpr u64 SubmeshMaterial = 0x10;
constexpr u64 SubmeshTriangles = 0x1c;
constexpr u64 SubmeshFirstIndex = 0x20;
// The element bits of a shape that choose a mesh particle's programs.
constexpr u32 ElementsColorBit = 3;
constexpr u32 ElementsTangentBit = 14;
// A G2_BG_Main material, as FUN_01401c50 reads it for mesh particles.
constexpr u64 SurfaceProgram = 0x10;
constexpr u64 SurfacePunchThrough = 0x14;
constexpr u64 SurfaceFlags = 0x28;
constexpr u32 SurfaceFlagPunchThrough = 2;
constexpr u64 SurfaceValues = 0x2c;
constexpr u64 SurfaceEmissive = 0x38;
constexpr u64 SurfaceLayeredValues = 0x3c;
constexpr u64 SurfaceLayeredEmissive = 0x4c;
constexpr u64 SurfaceHatching = 0x50;
constexpr u64 SurfaceColorScale = 0x54;
constexpr u64 SurfaceLayeredColorScale = 0x60;
constexpr u64 SurfaceColorMap = 0x20;
constexpr u64 SurfaceNormalMap = 0x70;
constexpr u64 SurfaceMask = 0x78;
constexpr u64 SurfaceLayeredColorMap = 0x80;
constexpr u64 SurfaceLayeredNormalMap = 0x88;
constexpr float SurfacePunchThroughLeast = 0.005f;
constexpr float SurfacePunchThroughMost = 0.995f;
constexpr float SurfaceEmissiveScale = 1.0f / 6.0f;
constexpr float SurfaceLastConstant = 3.0f;
// A map of such a material: its own texture or else a shared one, each with its object at +8, and
// its transform.
constexpr u64 MapOwnTexture = 0x20;
constexpr u64 MapSharedTexture = 0x18;
constexpr u64 MapOffset = 0x70;
constexpr u64 MapRepeat = 0x68;
// The block of camera values, as FUN_014b4100 reads it.
constexpr u64 ContextView = 0x0;
constexpr u64 ContextCamera = 0x40;
constexpr u64 ContextProjection = 0x50;
constexpr u64 ContextProjectionY = 0x64;
constexpr u64 ContextCascadeViews = 0x140;
constexpr u64 ContextCascadeProjections = 0x200;
constexpr u64 ContextCascadeStride = 0x40;
constexpr u64 ContextClip = 0xd0;
constexpr u64 ContextLight = 0x2c0;
constexpr u64 ContextLightDirection = 0x2d0;
constexpr u64 ContextLightLast = 0x2e0;
// The view's point lights the context holds; the lit programs take at most MaxLitPointLights of a
// cell's.
constexpr u64 ContextPointLightCount = 0x388;
constexpr u32 MaxLitPointLights = 10;
constexpr u32 LightPointLightsWord = 12;
constexpr u64 ContextScreen = 0x2f0;
// The fog's values, as FUN_014c0960 reads them.
constexpr u64 ContextFogMatrixRight = 0x300;
constexpr u64 ContextFogMatrixLeft = 0x340;
constexpr u64 ContextFogEnd = 0x38c;
constexpr u64 ContextFogValues = 0x390;
constexpr u64 ContextFogValue = 0x3a0;
constexpr u64 ContextFogScale = 0x3a4;
constexpr float FogScaleFactor = 0.1f;
// A material of the particle system.
constexpr u64 MaterialTextures = 0x58;
constexpr u64 MaterialMeshType = 0xa4;
constexpr s32 MeshTypeSurface = 0x4e;
constexpr u64 MaterialSurfaceVariant = 0xa8;
constexpr u64 MaterialDepthWrite = 0xb6;
constexpr u64 MaterialFxUsed = 0x70;
constexpr u64 MaterialAttribute = 0x80;
constexpr u64 MaterialAttribute2 = 0x90;
constexpr u64 MaterialDepthCompare = 0xb7;
constexpr u64 MaterialCull = 0xb8;
constexpr u64 MaterialLit = 0xc2;
constexpr u64 MaterialSoft = 0xc3;
constexpr u64 MaterialTarget = 0xc4;
constexpr u64 MaterialEnabled = 0xc5;
constexpr u8 TargetHalfSize = 9;
constexpr s32 NoTextureKey = -0x7ee3623b;
// The engine's manager: buffers and counts of the frame, and the compute object of the move.
constexpr u64 ManagerLibrary = 0x2c0;
constexpr u64 ManagerEffects = 0x300;
constexpr u64 ManagerOwners = 0x320;
constexpr u64 ManagerParticleCount = 0x168;
constexpr u64 ManagerObjectCount = 0x16c;
constexpr u64 ManagerEffectCount = 0x170;
constexpr u64 ManagerLiveEffects = 0x404;
constexpr u64 ManagerRing = 0x6f8;
constexpr u64 ManagerMove = 0x728;
// A compute object's first constants and their size in words.
constexpr u64 ComputeConstants = 0x500;
constexpr u64 ComputeConstantWords = 0x700;
// A buffer object: its descriptor and its data.
constexpr u64 BufferDescriptor = 0x28;
constexpr u64 DescriptorData = 0x10;
// The library: what it changes when effects load, and the buffers and curves the renderer mirrors.
constexpr u64 LibraryPending = 0x2050;
constexpr u64 LibraryApplied = 0x3058;
// Slots through which the game rewrites descriptions each frame, when they are on: which
// description, and whether the slot is in use.
constexpr u64 LibraryLiveOn = 0x4600;
constexpr u64 LibraryLiveIndex = 0x3244;
constexpr u64 LibraryLiveUsed = 0x324c;
constexpr u64 LibraryLiveStride = 0x50;
constexpr u64 LibraryDescriptions = 0x30a8;
constexpr u64 LibraryMaterials = 0x30e0;
constexpr u64 LibraryCurves = 0x31d8;
// Where __GLOBAL_CB__ of the game's move program lies, relative to the executable's start.
constexpr u64 NoiseTables = 0x1c7915c - 0x400000;

// The sampled meshes the log names, from the first on.
constexpr u32 LoggedSamples = 32;
u32 g_logged_samples = 0;

u32 g_generation = 1;
u32 g_curves = 0;
u32 g_loaded_curves = 0;
bool g_logged = false;
// A census of the game's items per mode and kind, for the log.
constexpr u32 CensusModes = 32;
constexpr u32 CensusKinds = 8;
constexpr u32 CensusFrames = 600;
std::array<std::array<u32, CensusKinds>, CensusModes> g_census_items{};
std::array<std::array<u32, CensusKinds>, CensusModes> g_census_instances{};
u32 g_census_frames = 0;

Matrix Transposed(const Matrix& m) {
    Matrix out;
    for (u32 row = 0; row < 4; row++) {
        for (u32 column = 0; column < 4; column++) {
            out[row * 4 + column] = m[column * 4 + row];
        }
    }
    return out;
}

VAddr BufferData(VAddr buffer_object) {
    const VAddr descriptor = Load<VAddr>(buffer_object + BufferDescriptor);
    return descriptor != 0 ? Load<VAddr>(descriptor + DescriptorData) : 0;
}

VAddr ReferenceTexture(VAddr reference) {
    const VAddr loaded = Load<VAddr>(reference);
    if (loaded == 0 || Load<u32>(loaded + ReferenceKind) != ReferenceLoaded) {
        return 0;
    }
    const VAddr object = Load<VAddr>(loaded + ReferenceObject);
    return object != 0 ? object + ObjectOfReference : 0;
}

// A material's texture of slot i: 0 for white, as the game binds its default.
u32 MaterialTexture(VAddr material, u32 i) {
    const VAddr references = Load<VAddr>(material + MaterialTextures);
    const VAddr reference = Load<VAddr>(references + i * 8);
    if (reference == 0 || Load<s32>(reference + 8) == NoTextureKey) {
        return 0;
    }
    const VAddr object = ReferenceTexture(reference);
    return object != 0 ? Textures::Get(object) : 0;
}

// The pixel program of an item, as FUN_014c0960 and FUN_014bc300 choose it; Count when it is not
// ported. The fog's low path (passes 10 to 13) takes the programs of passes 0, 1, 16 and 17.
Scene::ParticleProgram PixelProgram(u32 mode, VAddr material) {
    const u32 soft = Load<u8>(material + MaterialSoft) != 0 ? 1 : 0;
    const u32 variant = (Load<u8>(material + MaterialLit) != 0 ? 2 : 0) + soft;
    if (mode == 0 || mode == 1 || mode == 10 || mode == 11 || (mode >= 18 && mode <= 20)) {
        return static_cast<Scene::ParticleProgram>(variant);
    }
    if (mode == 14 || mode == 15) {
        return static_cast<Scene::ParticleProgram>(static_cast<u32>(Scene::ParticleProgram::Fog) +
                                                   variant);
    }
    if (mode == 12 || mode == 16) {
        return static_cast<Scene::ParticleProgram>(
            static_cast<u32>(Scene::ParticleProgram::Distortion) + soft);
    }
    if (mode == 13 || mode == 17) {
        return static_cast<Scene::ParticleProgram>(static_cast<u32>(Scene::ParticleProgram::Blur) +
                                                   soft);
    }
    return Scene::ParticleProgram::Count;
}

// The objects of a model item.
struct ModelObjectList {
    VAddr first = 0;
    u32 count = 0;
};

ModelObjectList FindModel(VAddr system, u32 item_key, u32 resource_key) {
    const VAddr end = Load<VAddr>(system + SystemModelsEnd);
    for (VAddr at = Load<VAddr>(system + SystemModels); at != end; at += 8) {
        const VAddr entry = Load<VAddr>(at);
        if (Load<s32>(entry + ModelEntryKey) != static_cast<s32>(item_key) ||
            Load<s32>(entry + ModelEntryResource) != static_cast<s32>(resource_key)) {
            continue;
        }
        const VAddr model = Load<VAddr>(entry);
        if (model == 0) {
            return {};
        }
        const VAddr first = Load<VAddr>(model + ModelObjects);
        return {first,
                static_cast<u32>((Load<VAddr>(model + ModelObjectsEnd) - first) / sizeof(VAddr))};
    }
    return {};
}

// The shape an object draws, or 0.
VAddr ObjectShape(VAddr object) {
    const VAddr at = Load<VAddr>(object + ObjectShapes) +
                     static_cast<s64>(Load<s32>(object + ObjectShapeOffset));
    return at != 0 ? Load<VAddr>(at) : 0;
}

// A map's handle; 0 for white or for a texture the renderer does not hold.
u32 MapHandle(VAddr map) {
    if (map == 0) {
        return 0;
    }
    VAddr texture = Load<VAddr>(map + MapOwnTexture);
    if (texture == 0) {
        texture = Load<VAddr>(map + MapSharedTexture);
    }
    return texture != 0 ? Textures::Get(texture + ObjectOfReference) : 0;
}

// A map's transform, as the programs read it: offset, then repeat.
void ReadMapTransform(VAddr map, float* out) {
    if (map == 0) {
        return;
    }
    std::memcpy(out, reinterpret_cast<const void*>(map + MapOffset), 2 * sizeof(float));
    std::memcpy(out + 2, reinterpret_cast<const void*>(map + MapRepeat), 2 * sizeof(float));
}

// The gradation at low and high when high does not lie under low; returns whether it does not.
bool ReadGradation(VAddr low_at, VAddr high_at, float* out) {
    const auto low = Load<std::array<float, 4>>(low_at);
    auto high = Load<std::array<float, 4>>(high_at);
    if (high[3] < low[3]) {
        return false;
    }
    const float span = high[3] - low[3];
    high[3] = span <= GradationLeast ? 0.0f : 1.0f / span;
    std::memcpy(out, low.data(), sizeof(low));
    std::memcpy(out + 4, high.data(), sizeof(high));
    return true;
}

// The game's constants of a mesh particle in the world's surfaces, as FUN_01401c50 builds them:
// three identity matrices, since the particle's own come from the simulation, and the gradation.
void WriteSurfaceMaterial(Scene::ParticleDraw& record, VAddr material, VAddr object, VAddr shape,
                          VAddr system) {
    std::array<float, 56> vertex{};
    for (u32 matrix = 0; matrix < 3; matrix++) {
        for (u32 d = 0; d < 4; d++) {
            vertex[matrix * 16 + d * 5] = 1.0f;
        }
    }
    if (!ReadGradation(object + ObjectGradationLow, object + ObjectGradationHigh, &vertex[48]) &&
        !ReadGradation(shape + ShapeGradationLow, shape + ShapeGradationHigh, &vertex[48])) {
        for (u32 i = 0; i < 3; i++) {
            vertex[48 + i] = GradationNone;
            vertex[52 + i] = GradationNone;
        }
    }
    std::memcpy(record.vertex_constants, vertex.data(), sizeof(vertex));

    const VAddr color_map = Load<VAddr>(material + SurfaceColorMap);
    const VAddr normal_map = Load<VAddr>(material + SurfaceNormalMap);
    const VAddr mask = Load<VAddr>(material + SurfaceMask);
    const VAddr layered_color = Load<VAddr>(material + SurfaceLayeredColorMap);
    const VAddr layered_normal = Load<VAddr>(material + SurfaceLayeredNormalMap);
    std::array<float, 40> pixel{};
    std::memcpy(&pixel[0], reinterpret_cast<const void*>(material + SurfaceColorScale),
                3 * sizeof(float));
    pixel[3] = (Load<u32>(material + SurfaceFlags) & SurfaceFlagPunchThrough) != 0
                   ? std::clamp(Load<float>(material + SurfacePunchThrough),
                                SurfacePunchThroughLeast, SurfacePunchThroughMost)
                   : -1.0f;
    std::memcpy(&pixel[4], reinterpret_cast<const void*>(material + SurfaceLayeredColorScale),
                3 * sizeof(float));
    pixel[7] = 1.0f;
    ReadMapTransform(color_map, &pixel[8]);
    ReadMapTransform(layered_color, &pixel[12]);
    ReadMapTransform(normal_map, &pixel[16]);
    ReadMapTransform(layered_normal, &pixel[20]);
    ReadMapTransform(mask, &pixel[24]);
    // The system's attribute fades the particle out by the screen door.
    const float remaining = 1.0f - Load<float>(system + SystemAttribute);
    pixel[28] = 1.0f - remaining * remaining * remaining * remaining;
    std::memcpy(&pixel[29], reinterpret_cast<const void*>(material + SurfaceValues),
                3 * sizeof(float));
    pixel[32] = Load<float>(material + SurfaceEmissive) * SurfaceEmissiveScale;
    std::memcpy(&pixel[33], reinterpret_cast<const void*>(material + SurfaceLayeredValues),
                4 * sizeof(float));
    pixel[37] = Load<float>(material + SurfaceLayeredEmissive) * SurfaceEmissiveScale;
    pixel[38] = Load<float>(material + SurfaceHatching);
    pixel[39] = SurfaceLastConstant;
    std::memcpy(record.pixel_constants, pixel.data(), sizeof(pixel));

    record.maps[0] = MapHandle(color_map);
    record.maps[1] = MapHandle(normal_map);
    record.maps[2] = MapHandle(mask);
    record.maps[3] = MapHandle(layered_color);
    record.maps[4] = MapHandle(layered_normal);
}

// The light's constants, as FUN_014c0960 builds them for its lit programs: its first values, its
// direction through the rotation at ContextProjection, its last values, and how many of a cell's
// point lights to take, none when the packet holds no grid.
void WriteLight(VAddr context, u8* packet, bool has_grid) {
    std::array<float, 16> light{};
    std::memcpy(&light[0], reinterpret_cast<const void*>(context + ContextLight),
                4 * sizeof(float));
    const auto direction = Load<std::array<float, 3>>(context + ContextLightDirection);
    const Matrix rotation = Load<Matrix>(context + ContextProjection);
    std::array<float, 3> turned{};
    for (u32 i = 0; i < 3; i++) {
        turned[i] = direction[0] * rotation[i] + direction[1] * rotation[4 + i] +
                    direction[2] * rotation[8 + i];
    }
    const float length =
        std::sqrt(turned[0] * turned[0] + turned[1] * turned[1] + turned[2] * turned[2]);
    for (u32 i = 0; i < 3; i++) {
        light[4 + i] = length > 0.0f ? turned[i] / length : 0.0f;
    }
    std::memcpy(&light[8], reinterpret_cast<const void*>(context + ContextLightLast),
                4 * sizeof(float));
    const u32 point_lights =
        has_grid ? std::min(Load<u32>(context + ContextPointLightCount), MaxLitPointLights) : 0;
    std::memcpy(&light[LightPointLightsWord], &point_lights, sizeof(point_lights));
    std::memcpy(packet + Scene::ParticleLightOffset, light.data(), sizeof(light));
}

// The fog's constants, as FUN_014c0960 builds them for its fog programs.
void WriteFog(VAddr context, u8* packet) {
    const float clip_x = Load<float>(context + ContextClip);
    const float clip_y = Load<float>(context + ContextClip + 4);
    const float scale = 1.0f / (clip_x * (clip_y + clip_y));
    const float linear_a = (clip_x - clip_y) * scale;
    const float linear_b = (clip_y + clip_x) * scale;
    const float reach = Load<float>(context + ContextFogEnd) - clip_x;
    std::array<float, Scene::ParticleFogWords> fog{};
    fog[0] = linear_a;
    fog[1] = linear_b;
    fog[2] = clip_x;
    fog[3] = 1.0f / reach;
    std::memcpy(&fog[4], reinterpret_cast<const void*>(context + ContextFogValues),
                4 * sizeof(float));
    fog[8] = Load<float>(context + ContextFogValue);
    fog[9] = Load<float>(context + ContextFogScale) * FogScaleFactor;
    fog[10] = reach;
    std::memcpy(&fog[12], reinterpret_cast<const void*>(context + ContextCamera),
                3 * sizeof(float));
    fog[15] = 1.0f;
    fog[16] = 1.0f / Load<float>(context + ContextProjection);
    fog[17] = 1.0f / Load<float>(context + ContextProjectionY);
    fog[18] = linear_a;
    fog[19] = linear_b;
    const Matrix matrix = Multiply(Load<Matrix>(context + ContextFogMatrixLeft),
                                   Load<Matrix>(context + ContextFogMatrixRight));
    std::memcpy(&fog[20], matrix.data(), sizeof(matrix));
    std::memcpy(packet + Scene::ParticleFogOffset, fog.data(), sizeof(fog));
}

// Copies the descriptions the game has just rewritten.
void WriteLiveDescriptions(VAddr library, u8* packet, Scene::ParticleHeader& out) {
    if (Load<u8>(library + LibraryLiveOn) == 0 || out.description_source == 0) {
        return;
    }
    auto* indices = reinterpret_cast<u32*>(packet + Scene::ParticleLiveIndicesOffset);
    for (u32 slot = 0; slot < Scene::MaxParticleLiveDescriptions; slot++) {
        if (Load<s32>(library + LibraryLiveUsed + slot * LibraryLiveStride) == 0) {
            continue;
        }
        const u32 index = Load<u32>(library + LibraryLiveIndex + slot * LibraryLiveStride);
        indices[out.live_descriptions] = index;
        std::memcpy(packet + Scene::ParticleLiveOffset +
                        out.live_descriptions * Scene::ParticleDescriptionBytes,
                    reinterpret_cast<const void*>(out.description_source +
                                                  index * Scene::ParticleDescriptionBytes),
                    Scene::ParticleDescriptionBytes);
        out.live_descriptions++;
    }
}

// The values of a frame's camera that all its draws hold.
struct Camera {
    Matrix view;
    Matrix projection;
    Matrix view_inverse;
    Matrix projection_inverse;
    std::array<float, 2> screen;
    std::array<float, 2> clip;
};

// Writes the game's PfxBufferCommon and the handles of the textures, in order: the packet is
// write-combined.
void WriteDraw(Scene::ParticleDraw& record, const Camera& camera, VAddr material, VAddr system,
               u32 first, u32 instances) {
    std::memcpy(&record.constants[0], camera.view.data(), sizeof(camera.view));
    std::memcpy(&record.constants[16], camera.projection.data(), sizeof(camera.projection));
    std::memcpy(&record.constants[32], camera.view_inverse.data(), sizeof(camera.view_inverse));
    std::memcpy(&record.constants[48], camera.projection_inverse.data(),
                sizeof(camera.projection_inverse));
    const float scale = Load<u8>(material + MaterialTarget) == TargetHalfSize ? 0.5f : 1.0f;
    record.constants[64] = camera.screen[0] * scale;
    record.constants[65] = camera.screen[1] * scale;
    record.constants[66] = camera.screen[0] * scale;
    record.constants[67] = camera.screen[1] * scale;
    record.constants[68] = camera.clip[0];
    record.constants[69] = camera.clip[1];
    record.constants[70] = camera.clip[1] - camera.clip[0];
    record.constants[71] = 0.0f;
    std::memcpy(&record.constants[72], reinterpret_cast<const void*>(material + MaterialAttribute),
                4 * sizeof(float));
    std::memcpy(&record.constants[76], reinterpret_cast<const void*>(material + MaterialAttribute2),
                3 * sizeof(float));
    const u32 attribute = Load<u32>(system + SystemAttribute);
    std::memcpy(&record.constants[79], &attribute, sizeof(attribute));
    std::memcpy(&record.constants[80], &first, sizeof(first));
    std::memcpy(&record.constants[81], &instances, sizeof(instances));
    record.constants[82] = 0.0f;
    record.constants[83] = 0.0f;
    std::memcpy(&record.constants[84], reinterpret_cast<const void*>(material + MaterialFxUsed),
                4 * sizeof(float));
    for (u32 t = 0; t < 4; t++) {
        record.textures[t] = MaterialTexture(material, t);
    }
}

// The offset of a semantic's element in a vertex buffer object, or 0 when it has none.
u32 ElementOffset(VAddr vertex_buffer, u32 semantic) {
    const u32 element = Load<u8>(vertex_buffer + VertexBufferSemantics + semantic);
    return element < VertexBufferNoElement
               ? Load<u8>(vertex_buffer + VertexBufferOffsets + u64{element} * 4)
               : 0;
}

// Adds a mesh that places particles on itself, with the constants FUN_014b3860 builds.
void AddSample(u8* packet, Scene::ParticleHeader& out, u64& palette_bytes,
               const std::array<float, 12>& matrix, VAddr vertex_buffer, VAddr object, u32 emitter,
               u32 kind, VAddr skin_buffer, VAddr palette, u32 bones) {
    const u32 vertices = Load<u32>(vertex_buffer + VertexBufferVertices);
    // The game's buffers read 0 past their ends, which leaves the program with nothing to do. The
    // renderer's are plain memory.
    if (vertices == 0 || emitter >= out.objects) {
        return;
    }
    const u32 stride = Load<u16>(vertex_buffer + VertexBufferStride);
    const Meshes::Mesh* mesh = Meshes::Get(vertex_buffer - ShapeVertexBufferObject);
    const bool skinned = skin_buffer != 0 && palette != 0 && bones != 0;
    const u64 skin_bytes = skinned ? u64{bones} * BoneBytes : 0;
    if (mesh == nullptr || out.samples == Scene::MaxParticleSamples ||
        u64{vertices} * stride > mesh->vertex_bytes || (skinned && mesh->skin == 0) ||
        palette_bytes + skin_bytes > Scene::MaxParticleSamplePaletteBytes) {
        out.dropped_samples++;
        return;
    }
    if (g_logged_samples < LoggedSamples) {
        g_logged_samples++;
        LOG_INFO(Render_Vulkan,
                 "GR2port particles: mesh {:#x} places particles: emitter {} of {}, {} vertices of "
                 "{} bytes, kind {}, {} bones",
                 vertex_buffer, emitter, out.objects, vertices, stride, kind, skinned ? bones : 0);
    }
    std::array<u32, 28> constants{};
    std::memcpy(constants.data(), matrix.data(), sizeof(matrix));
    constants[12] = vertices;
    constants[13] = Load<u32>(object + ObjectIdentity);
    constants[16] = stride;
    constants[20] = ElementOffset(vertex_buffer, SemanticPosition);
    constants[21] = ElementOffset(vertex_buffer, SemanticNormal);
    constants[24] = emitter;
    constants[25] = kind;
    if (skinned) {
        constants[17] = Load<u16>(skin_buffer + VertexBufferStride);
        constants[22] = ElementOffset(skin_buffer, SemanticWeights);
        constants[23] = ElementOffset(skin_buffer, SemanticBones);
    }
    auto* samples = reinterpret_cast<Scene::ParticleSample*>(packet + Scene::ParticleSamplesOffset);
    Scene::ParticleSample& sample = samples[out.samples++];
    std::memcpy(sample.constants, constants.data(), sizeof(constants));
    sample.vertices = mesh->vertices;
    sample.weights = skinned ? mesh->skin : 0;
    sample.palette_offset = Scene::ParticleSamplePalettesOffset + palette_bytes;
    if (skinned) {
        std::memcpy(packet + Scene::ParticleSamplePalettesOffset + palette_bytes,
                    reinterpret_cast<const void*>(palette), skin_bytes);
        palette_bytes += skin_bytes;
    }
}

// The meshes that place particles on themselves: the view's list, then the models of effects that
// the list does not name, with their vertices as they are.
void WriteSamples(VAddr fx, VAddr view, u8* packet, Scene::ParticleHeader& out) {
    u64 palette_bytes = 0;
    const VAddr records = Load<VAddr>(view + ViewSamples);
    const u32 count = records != 0 ? Load<u32>(view + ViewSampleCount) : 0;
    for (u32 i = 0; i < count; i++) {
        const VAddr record = records + u64{i} * SampleBytes;
        const VAddr vertex_buffer = Load<VAddr>(record + SampleVertexBuffer);
        const VAddr object = Load<VAddr>(record + SampleObject);
        if (vertex_buffer == 0 || object == 0) {
            continue;
        }
        VAddr skin_buffer = 0;
        VAddr palette = 0;
        u32 bones = 0;
        const s32 skin = Load<s32>(record + SampleSkin);
        if (skin >= 0) {
            const VAddr entry =
                Load<VAddr>(view + ViewSampleSkins) + static_cast<u64>(skin) * SkinEntryBytes;
            skin_buffer = Load<VAddr>(entry);
            palette = Load<VAddr>(entry + SkinEntryPalette);
            bones = Load<u32>(entry + SkinEntryBones);
        }
        const u32 emitter =
            Load<u32>(record + SampleEmitter) - Load<u8>(record + SampleEmitterOffset);
        AddSample(packet, out, palette_bytes, Load<std::array<float, 12>>(record), vertex_buffer,
                  object, emitter, Load<u8>(record + SampleKind), skin_buffer, palette, bones);
    }
    constexpr std::array<float, 12> Identity{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                                             0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const VAddr effects_end = Load<VAddr>(fx + FxModelEffectsEnd);
    for (VAddr at = Load<VAddr>(fx + FxModelEffects); at != effects_end; at += sizeof(VAddr)) {
        const VAddr effect = Load<VAddr>(at);
        const u8 flags = Load<u8>(effect + ModelEffectFlags);
        if (flags == 0 || (flags & ModelEffectSkippedFlags) != 0) {
            continue;
        }
        const VAddr emitter = Load<VAddr>(effect + ModelEffectEmitter);
        const VAddr system = Load<VAddr>(emitter + EmitterSystem);
        const VAddr models_end = Load<VAddr>(system + SystemModelsEnd);
        for (VAddr model = Load<VAddr>(system + SystemModels); model != models_end;
             model += sizeof(VAddr)) {
            const VAddr entry = Load<VAddr>(model);
            if (entry == 0 || Load<u8>(entry + ModelEntryOff) != 0) {
                continue;
            }
            const u32 kind = Load<u32>(entry + ModelEntryKind);
            const VAddr object = Load<VAddr>(entry + ModelEntryObject);
            if ((kind | 2) != ModelEntryKindPlaced || object == 0) {
                continue;
            }
            bool listed = false;
            for (u32 i = 0; i < count && !listed; i++) {
                listed = Load<VAddr>(records + u64{i} * SampleBytes + SampleObject) == object;
            }
            const VAddr shape = listed ? 0 : ObjectShape(object);
            if (shape != 0) {
                const u32 base =
                    Load<u32>(emitter + EmitterObjectBase) - Load<u32>(entry + ModelEntryKey);
                AddSample(packet, out, palette_bytes, Identity, shape + ShapeVertexBufferObject,
                          object, base, kind, 0, 0, 0);
            }
        }
    }
}

// What the commands of a run share.
struct RunState {
    u32 pipeline;
    u32 mode;
    u32 tested;
    u32 write;
    u32 cull;
};

// Adds a frame's draws to the packet.
class DrawWriter {
public:
    DrawWriter(u8* packet, Scene::ParticleHeader& out_)
        : out{out_},
          draws{reinterpret_cast<Scene::ParticleDraw*>(packet + Scene::ParticleDrawsOffset)},
          commands{reinterpret_cast<Scene::VertexCommand*>(packet + Scene::ParticleCommandsOffset)},
          indexed_commands{
              reinterpret_cast<Scene::Command*>(packet + Scene::ParticleIndexedCommandsOffset)},
          runs{reinterpret_cast<Scene::ParticleRun*>(packet + Scene::ParticleRunsOffset)} {}

    void AddSprite(const RunState& state, const Camera& camera, VAddr material, VAddr system,
                   u32 first, u32 instances, u32 vertices) {
        if (out.draws == Scene::MaxParticleDraws) {
            out.dropped++;
            return;
        }
        const u32 draw = out.draws++;
        WriteDraw(draws[draw], camera, material, system, first, instances);
        const u32 command = out.vertex_commands++;
        commands[command] = {
            .vertex_count = vertices,
            .instance_count = instances,
            .first_vertex = 0,
            .first_instance = draw << Scene::ParticleDrawShift,
        };
        AddCommand(state, 0, command);
    }

    // A draw per object of the model's set, and a command per submesh of its shape.
    void AddModel(const RunState& state, const Camera& camera, const ModelObjectList& objects,
                  VAddr material, VAddr system, u32 first, u32 instances, bool needs_elements) {
        for (u32 o = 0; o < objects.count; o++) {
            const VAddr object = Load<VAddr>(objects.first + u64{o} * sizeof(VAddr));
            const VAddr shape = object != 0 ? ObjectShape(object) : 0;
            if (shape == 0 || (needs_elements && (Load<u16>(shape + ShapeElements) &
                                                  ModelElements) != ModelElements)) {
                continue;
            }
            const Meshes::Mesh* mesh = Meshes::Get(shape);
            if (mesh == nullptr || out.draws == Scene::MaxParticleDraws) {
                out.dropped++;
                continue;
            }
            const u32 draw = out.draws++;
            WriteDraw(draws[draw], camera, material, system, first, instances);
            WriteMesh(draws[draw], *mesh, objects.count, o);
            const u32 submeshes = Load<u16>(shape + ShapeSubmeshCount);
            for (u32 s = 0; s < submeshes; s++) {
                const VAddr submesh = Load<VAddr>(shape + ShapeSubmeshes + u64{s} * sizeof(VAddr));
                if (submesh != 0) {
                    AddSubmesh(state, *mesh, submesh, draw, instances);
                }
            }
        }
    }

    // A draw per submesh of each object whose shape fits the material's programs, since each
    // submesh has a material of the game's world.
    void AddSurfaces(const RunState& state, const Camera& camera, const ModelObjectList& objects,
                     VAddr material, VAddr system, u32 first, u32 instances) {
        for (u32 o = 0; o < objects.count; o++) {
            const VAddr object = Load<VAddr>(objects.first + u64{o} * sizeof(VAddr));
            const VAddr shape = object != 0 ? ObjectShape(object) : 0;
            if (shape == 0) {
                continue;
            }
            const u32 elements = Load<u16>(shape + ShapeElements);
            const u32 variant =
                ((elements >> ElementsColorBit) & 1) << 1 | ((elements >> ElementsTangentBit) & 1);
            if (variant != state.pipeline) {
                continue;
            }
            const Meshes::Mesh* mesh = Meshes::Get(shape);
            if (mesh == nullptr) {
                out.dropped++;
                continue;
            }
            const u32 submeshes = Load<u16>(shape + ShapeSubmeshCount);
            for (u32 s = 0; s < submeshes; s++) {
                const VAddr submesh = Load<VAddr>(shape + ShapeSubmeshes + u64{s} * sizeof(VAddr));
                const VAddr surface = submesh != 0 ? Load<VAddr>(submesh + SubmeshMaterial) : 0;
                if (surface == 0 || Load<s32>(surface + SurfaceProgram) < 0) {
                    continue;
                }
                if (out.draws == Scene::MaxParticleDraws) {
                    out.dropped++;
                    continue;
                }
                const u32 draw = out.draws++;
                Scene::ParticleDraw& record = draws[draw];
                WriteDraw(record, camera, material, system, first, instances);
                WriteMesh(record, *mesh, objects.count, o);
                WriteSurfaceMaterial(record, surface, object, shape, system);
                AddSubmesh(state, *mesh, submesh, draw, instances);
            }
        }
    }

    void Finish() {
        EndRun();
    }

private:
    void WriteMesh(Scene::ParticleDraw& record, const Meshes::Mesh& mesh, u32 objects, u32 object) {
        record.model_count[0] = static_cast<float>(objects);
        record.model_count[1] = static_cast<float>(object);
        record.mesh.vertices = mesh.vertices;
        record.mesh.stride = mesh.stride;
        record.mesh.position = mesh.position;
        record.mesh.texcoord = mesh.texcoord;
        record.mesh.texcoord1 = mesh.texcoord1;
        record.mesh.normal = mesh.normal;
        record.mesh.tangent = mesh.tangent;
        record.mesh.vertex_color = mesh.vertex_color;
    }

    void AddSubmesh(const RunState& state, const Meshes::Mesh& mesh, VAddr submesh, u32 draw,
                    u32 instances) {
        const u32 first_index = Load<u32>(submesh + SubmeshFirstIndex);
        const u32 index_count = u32{Load<u16>(submesh + SubmeshTriangles)} * 3;
        if (u64{first_index} + index_count > mesh.indices ||
            out.indexed_commands == Scene::MaxParticleDraws) {
            out.dropped++;
            return;
        }
        const u32 command = out.indexed_commands++;
        indexed_commands[command] = {
            .index_count = index_count,
            .instance_count = instances,
            .first_index = mesh.first_index + first_index,
            .vertex_offset = 0,
            .first_instance = draw << Scene::ParticleDrawShift,
        };
        AddCommand(state, 1, command);
    }

    void EndRun() {
        if (run.count == 0) {
            return;
        }
        std::memcpy(&runs[out.runs++], &run, sizeof(run));
        if (run.mode >= Scene::ParticleShadowMode) {
            out.shadow_runs++;
        } else if (run.mode == Scene::ParticleSurfaceMode) {
            out.surface_runs++;
        }
        run = {};
    }

    void AddCommand(const RunState& state, u32 indexed, u32 command) {
        if (run.count != 0 && (run.pipeline != state.pipeline || run.mode != state.mode ||
                               run.tested != state.tested || run.write != state.write ||
                               run.cull != state.cull || run.indexed != indexed)) {
            EndRun();
        }
        if (run.count == 0) {
            run.pipeline = state.pipeline;
            run.mode = state.mode;
            run.first = command;
            run.tested = state.tested;
            run.write = state.write;
            run.cull = state.cull;
            run.indexed = indexed;
        }
        run.count++;
    }

    Scene::ParticleHeader& out;
    Scene::ParticleDraw* draws;
    Scene::VertexCommand* commands;
    Scene::Command* indexed_commands;
    Scene::ParticleRun* runs;
    Scene::ParticleRun run{};
};

void LogCensus(const Scene::ParticleHeader& out) {
    if (++g_census_frames != CensusFrames) {
        return;
    }
    std::string text;
    for (u32 mode = 0; mode < CensusModes; mode++) {
        for (u32 kind = 0; kind < CensusKinds; kind++) {
            if (g_census_items[mode][kind] != 0) {
                text += fmt::format(" m{}k{}:{:.1f}x{:.0f}", mode, kind,
                                    static_cast<double>(g_census_items[mode][kind]) / CensusFrames,
                                    static_cast<double>(g_census_instances[mode][kind]) /
                                        g_census_items[mode][kind]);
            }
        }
    }
    LOG_INFO(Render_Vulkan,
             "GR2port particles: items per frame x instances:{}; {} live descriptions, {} curves "
             "made, {} curves from loaded textures, {} meshes place particles, {} refused",
             text, out.live_descriptions, g_curves, g_loaded_curves, out.samples,
             out.dropped_samples);
    g_census_items = {};
    g_census_instances = {};
    g_census_frames = 0;
}

} // namespace

void OnSetup(VAddr fx) {
    const VAddr renderer = Load<VAddr>(fx + FxRenderer);
    if (renderer == 0) {
        return;
    }
    const VAddr manager = Load<VAddr>(renderer + RendererManager);
    const VAddr library = manager != 0 ? Load<VAddr>(manager + ManagerLibrary) : 0;
    if (library != 0 &&
        (Load<u32>(library + LibraryPending) != 0 || Load<u32>(library + LibraryApplied) != 0)) {
        g_generation++;
    }
    const VAddr first = Load<VAddr>(fx + FxCurves);
    const VAddr last = Load<VAddr>(fx + FxCurvesEnd);
    for (VAddr at = first; at < last; at += 8) {
        const VAddr curve = Load<VAddr>(at);
        if (curve == 0) {
            continue;
        }
        if (Load<VAddr>(curve) != 0) {
            g_loaded_curves++;
            continue;
        }
        const VAddr texture = Load<VAddr>(curve + CurveTexture);
        const u32 columns = Load<u32>(curve + CurveColumns);
        const VAddr memory = texture != 0 ? Load<VAddr>(texture + TextureMemory) : 0;
        const VAddr texels = memory != 0 ? Load<VAddr>(memory) : 0;
        if (texels != 0 && columns != 0) {
            Particles::QueueCurve(Load<u32>(curve + CurveDestination), columns, texels,
                                  Load<u32>(texture + TextureBytes) >> TextureRowShift);
            g_curves++;
        }
    }
}

void Write(VAddr fx, VAddr view, u8* packet, Scene::Header& header) {
    const VAddr renderer = Load<VAddr>(fx + FxRenderer);
    const VAddr manager = renderer != 0 ? Load<VAddr>(renderer + RendererManager) : 0;
    const VAddr library = manager != 0 ? Load<VAddr>(manager + ManagerLibrary) : 0;
    const VAddr context = renderer != 0 ? Load<VAddr>(renderer + RendererContext) : 0;
    const VAddr table = renderer != 0 ? Load<VAddr>(renderer + RendererTable) : 0;
    if (library == 0 || context == 0 || table == 0) {
        return;
    }
    Scene::ParticleHeader out{};
    out.generation = g_generation;
    out.description_source = BufferData(library + LibraryDescriptions);
    out.material_source = BufferData(library + LibraryMaterials);
    out.curve_source = Load<VAddr>(library + LibraryCurves);
    out.noise_source = MemoryPatcher::g_eboot_address + NoiseTables;

    // The frame's input to the simulation: the slot of the ring the game filled last.
    const VAddr ring = Load<VAddr>(manager + ManagerRing);
    const VAddr move = Load<VAddr>(manager + ManagerMove);
    const s32 ring_size = ring != 0 ? Load<s32>(ring + 4) : 0;
    if (ring_size <= 0 || move == 0) {
        out.refused = 1;
        header.particles = out;
        return;
    }
    const s32 ring_next = Load<s32>(ring);
    const u32 slot = static_cast<u32>((ring_next - 1 + ring_size) % ring_size);
    const VAddr effects = BufferData(Load<VAddr>(manager + ManagerEffects + slot * 8));
    const VAddr owners = BufferData(Load<VAddr>(manager + ManagerOwners + slot * 8));
    const u64 effect_bytes =
        u64{Load<u32>(manager + ManagerEffectCount)} * Scene::ParticleEffectBytes;
    out.objects = Load<u32>(manager + ManagerObjectCount);
    out.live_effects = Load<u16>(manager + ManagerLiveEffects);
    out.particle_bytes = u64{Load<u32>(manager + ManagerParticleCount)} * Scene::ParticleBytes;
    out.object_bytes = u64{out.objects} * Scene::ParticleObjectBytes;
    if (effects == 0 || owners == 0 || effect_bytes > Scene::MaxParticleEffectBytes ||
        out.objects > Scene::MaxParticleOwners) {
        out.refused = 1;
        header.particles = out;
        if (!g_logged) {
            g_logged = true;
            LOG_ERROR(Render_Vulkan,
                      "GR2port particles: the input does not fit: effects {:#x} ({} bytes), owners "
                      "{:#x} ({})",
                      effects, effect_bytes, owners, out.objects);
        }
        return;
    }
    std::memcpy(packet + Scene::ParticleEffectsOffset, reinterpret_cast<const void*>(effects),
                effect_bytes);
    std::memcpy(packet + Scene::ParticleOwnersOffset, reinterpret_cast<const void*>(owners),
                u64{out.objects} * sizeof(u64));
    // The move's constants, which the game has just set: the pool's size is among them.
    const u32 words = std::min(Load<u32>(move + ComputeConstantWords), Scene::ParticleSceneWords);
    std::memcpy(packet + Scene::ParticleSceneOffset,
                reinterpret_cast<const void*>(move + ComputeConstants), words * 4);
    out.particles = Load<u32>(move + ComputeConstants + Scene::ParticlePoolWord * 4);
    if (!g_logged) {
        g_logged = true;
        LOG_INFO(Render_Vulkan,
                 "GR2port particles: capacity {} particles, {} objects, {} effect bytes; pool {}, "
                 "{} live effects, slot {} of {}, {} constant words",
                 out.particle_bytes / Scene::ParticleBytes, out.objects, effect_bytes,
                 out.particles, out.live_effects, slot, ring_size, words);
    }

    WriteLight(context, packet, header.point_lights != 0);
    WriteFog(context, packet);
    WriteLiveDescriptions(library, packet, out);
    WriteSamples(fx, view, packet, out);

    // The frame's draws, in the game's order.
    // The programs multiply by columns: FUN_014b4100 ends by transposing what it took.
    const Matrix game_view = Load<Matrix>(context + ContextView);
    const Matrix game_projection = Load<Matrix>(context + ContextProjection);
    const Camera camera{
        .view = Transposed(game_view),
        .projection = Transposed(game_projection),
        .view_inverse = Transposed(Invert(game_view)),
        .projection_inverse = Transposed(Invert(game_projection)),
        .screen = Load<std::array<float, 2>>(context + ContextScreen),
        .clip = Load<std::array<float, 2>>(context + ContextClip),
    };
    // The shadow passes take a cascade's matrices, and no program reads their inverses.
    std::array<Camera, Scene::Cascades> cascades;
    for (u32 c = 0; c < Scene::Cascades; c++) {
        cascades[c] = camera;
        cascades[c].view =
            Transposed(Load<Matrix>(context + ContextCascadeViews + c * ContextCascadeStride));
        cascades[c].projection = Transposed(
            Load<Matrix>(context + ContextCascadeProjections + c * ContextCascadeStride));
    }
    DrawWriter writer{packet, out};
    const VAddr items = Load<VAddr>(table + TableItems);
    const u32 count = Load<u32>(table + TableCount);
    for (u32 i = 0; i < count; i++) {
        const VAddr item = Load<VAddr>(items + u64{i} * 8);
        const VAddr resource = Load<VAddr>(item + ItemResource);
        const VAddr emitter = Load<VAddr>(item + ItemEmitter);
        const u32 instances = Load<u32>(item + ItemCount);
        if (resource == 0 || emitter == 0 || instances == 0) {
            continue;
        }
        const VAddr material = Load<VAddr>(resource + ResourceMaterial);
        const VAddr system = Load<VAddr>(emitter + EmitterSystem);
        if (material == 0 || system == 0 || Load<u8>(material + MaterialEnabled) == 0 ||
            (Load<u8>(emitter + EmitterState) & EmitterStateMask) == EmitterStateHidden) {
            continue;
        }
        const u32 kind = Load<u32>(item + ItemKind);
        const u32 mode = Load<u32>(item + ItemMode);
        if (mode < CensusModes && kind < CensusKinds) {
            g_census_items[mode][kind]++;
            g_census_instances[mode][kind] += instances;
        }
        const u32 tested = Load<u8>(material + MaterialDepthCompare) != 0 ? 1 : 0;
        const u32 write = Load<u8>(material + MaterialDepthWrite) != 0 ? 1 : 0;
        const u32 cull = Load<u8>(material + MaterialCull);
        const u32 first = Load<u32>(item + ItemFirst);
        const auto model_objects = [&] {
            return FindModel(system, Load<u32>(item + ItemModelKey),
                             Load<u32>(resource + ResourceKey));
        };
        if (mode == ShadowPass) {
            if (kind != KindSprite && kind != KindModel) {
                out.unported++;
                continue;
            }
            // The game's shadow passes keep their own depth state.
            const Scene::ParticleShadowProgram program =
                kind == KindModel                        ? Scene::ParticleShadowProgram::Model
                : Load<u8>(material + MaterialSoft) != 0 ? Scene::ParticleShadowProgram::SpriteSoft
                                                         : Scene::ParticleShadowProgram::Sprite;
            for (u32 c = 0; c < Scene::Cascades; c++) {
                const RunState state{static_cast<u32>(program), Scene::ParticleShadowMode + c, 1, 1,
                                     cull};
                if (kind == KindSprite) {
                    writer.AddSprite(state, cascades[c], material, system, first, instances,
                                     SpriteVertices);
                } else {
                    writer.AddModel(state, cascades[c], model_objects(), material, system, first,
                                    instances, false);
                }
            }
            continue;
        }
        if (mode == Scene::ParticleSurfaceMode) {
            if (kind != KindMesh || Load<s32>(material + MaterialMeshType) != MeshTypeSurface) {
                out.unported++;
                continue;
            }
            const RunState state{Load<u32>(material + MaterialSurfaceVariant) &
                                     (Scene::ParticleSurfaceVariants - 1),
                                 mode, tested, write, cull};
            writer.AddSurfaces(state, camera, model_objects(), material, system, first, instances);
            continue;
        }
        const Scene::ParticleProgram program = PixelProgram(mode, material);
        if (program == Scene::ParticleProgram::Count ||
            (kind != KindSprite && kind != KindRibbon && kind != KindModel)) {
            out.unported++;
            continue;
        }
        const u32 shape = kind == KindSprite   ? static_cast<u32>(Scene::ParticleShape::Sprite)
                          : kind == KindRibbon ? static_cast<u32>(Scene::ParticleShape::Ribbon)
                                               : static_cast<u32>(Scene::ParticleShape::Model);
        const RunState state{shape * static_cast<u32>(Scene::ParticleProgram::Count) +
                                 static_cast<u32>(program),
                             mode, tested, write, cull};
        if (kind == KindModel) {
            writer.AddModel(state, camera, model_objects(), material, system, first, instances,
                            true);
        } else {
            writer.AddSprite(state, camera, material, system, first, instances,
                             kind == KindSprite ? SpriteVertices : RibbonVertices);
        }
    }
    writer.Finish();
    LogCensus(out);
    header.particles = out;
}

} // namespace Gr2Port::ParticleCapture
