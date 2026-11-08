// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/gr2port/capture.h"
#include "video_core/gr2port/capture_math.h"
#include "video_core/gr2port/deformers.h"
#include "video_core/gr2port/emulated_gpu.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/particle_capture.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/scene_ring.h"
#include "video_core/gr2port/textures.h"
#include "video_core/gr2port/window.h"

namespace Gr2Port::Capture {

namespace {

// View: item pool base, sort key count, key array end, per-category counts and first keys, and
// the list of BG_Main items drawn instanced.
constexpr u64 ViewItems = 0xb60;
constexpr u64 ViewKeyCount = 0xb58;
constexpr u64 ViewKeys = 0xb70;
constexpr u64 ViewCategoryCount = 0xecc;
constexpr u64 ViewCategoryFirst = 0xe34;
constexpr u64 ViewBatchList = 0xb80;
// View: the deformer records, in 0x40-byte slots from *(view + 0xbb8), the list of their slot
// indices at *(view + 0xbd0), and the list's range of each deformer kind as u16s from +0xfb0.
constexpr u64 ViewDeformers = 0xbb8;
constexpr u64 ViewDeformerList = 0xbd0;
constexpr u64 ViewDeformerRanges = 0xfb0;
constexpr u32 KindSkin = 0;
// Skin record: the instance's vertex buffer object +0x10, the skin vertex buffer object +0x18,
// the shape's vertex buffer object +0x20, palette +0x28, bone count +0x30, shape +0x38 and
// constants +0x40 {vertices, stride, normal offset, tangent offset}.
constexpr u64 SkinOutput = 0x10;
constexpr u64 SkinStream = 0x18;
constexpr u64 SkinSource = 0x20;
constexpr u64 SkinPalette = 0x28;
constexpr u64 SkinBones = 0x30;
constexpr u64 SkinShape = 0x38;
constexpr u64 SkinConstants = 0x40;
// The game's Skin program reads and writes positions as RGB32 FLOAT at offset 0.
constexpr u32 SkinPosition = 3u << 8;
constexpr u32 SkinGroupSize = 64;
constexpr u32 KindSoftBody = 1;
// SoftBody record: the instance's vertex buffer object +0x10, the shape's +0x20, a palette +0x28
// when Skin deforms the instance first (the anchors then come from the instance), the shape
// +0x38, the buffer objects of the particles +0x40, constraints +0x48 and vertex to particle map
// +0x50 (in the shape's SoftBody object at +0x1b0), and the constants of the reset program +0x60
// or of the simulate program +0x68, then of the write program +0x70.
constexpr u64 SoftBodyOutput = 0x10;
constexpr u64 SoftBodySource = 0x20;
constexpr u64 SoftBodyPalette = 0x28;
constexpr u64 SoftBodyShape = 0x38;
constexpr u64 SoftBodyParticles = 0x40;
constexpr u64 SoftBodyConstraints = 0x48;
constexpr u64 SoftBodyMap = 0x50;
constexpr u64 SoftBodyReset = 0x60;
constexpr u64 SoftBodySimulate = 0x68;
constexpr u64 SoftBodyWrite = 0x70;
constexpr u64 ShapeSoftBody = 0x1b0;
constexpr u64 ObjectParticles = 0x88;
constexpr u64 ObjectConstraints = 0xa0;
constexpr u64 ObjectMap = 0xb8;
// Constant words. Simulate: the grid matrix 0x10, stride 0x2d, particles 0x2e, iterations 0x2f,
// sphere count 0x31, depth fade 0x32 and 0x33. Reset: stride 0x11, particles 0x12. Write: vertices
// 0x10, stride 0x11.
constexpr u32 SimulateWords = 60;
constexpr u32 ResetWords = 20;
constexpr u32 WriteWords = 20;
constexpr u32 SimulateGrid = 0x10;
constexpr u32 SimulateStride = 0x2d;
constexpr u32 SimulateParticles = 0x2e;
constexpr u32 SimulateIterations = 0x2f;
constexpr u32 SimulateSpheres = 0x31;
constexpr u32 SimulateFade = 0x32;
constexpr u32 ResetStride = 0x11;
constexpr u32 ResetParticles = 0x12;
constexpr u32 WriteVertices = 0x10;
constexpr u32 WriteStride = 0x11;
// The game's programs read the normal at offset 12.
constexpr u32 MinSoftBodyStride = 16;
constexpr u32 MaxIterations = 16;
constexpr u32 SoftBodyGroupSize = 64;
// View: the near value n of the effector grid +0x120, the grid's clip matrix +0x130, the
// projection's x and y scales +0xa0 and +0xb4, and the effector spheres +0xc30, 32 bytes each,
// with their count +0xc38. The grid's depth runs from n to GridFar.
constexpr u64 ViewGridNear = 0x120;
constexpr u64 ViewGridMatrix = 0x130;
constexpr u64 ViewProjectionX = 0xa0;
constexpr u64 ViewProjectionY = 0xb4;
constexpr u64 ViewSpheres = 0xc30;
constexpr u64 ViewSphereCount = 0xc38;
constexpr u64 SphereBytes = 32;
constexpr float GridFar = 40.0f;
constexpr float GridFadeStart = 32.0f;
// View: the point lights +0xc10, 48 bytes each, with their count +0xc18, and how much the point
// lights fade where the key light is shadowed +0x890.
constexpr u64 ViewPointLights = 0xc10;
constexpr u64 ViewPointLightCount = 0xc18;
constexpr u64 ViewShadowInfluence = 0x890;
// View: V +0x20, inverse(V) +0x60, the cascade records from +0x400 every 0x110 ({valid, split,
// depth bias constant, depth bias slope} and the cascade VP at +0x90), and the casters'
// view-projection tables from +0x748 (a pointer per table and cascade).
constexpr u64 ViewMatrix = 0x20;
constexpr u64 ViewInverse = 0x60;
constexpr u64 ViewProjection = 0xa0;
constexpr u64 ViewCascades = 0x400;
constexpr u64 CascadeBytes = 0x110;
constexpr u64 CascadeBiasConstant = 0x08;
constexpr u64 CascadeBiasSlope = 0x0c;
constexpr u64 CascadeViewProjection = 0x90;
constexpr u64 ViewCasterTables = 0x748;
// View: the key light's colour times intensity +0x7b0 and the view-space direction its light
// travels in +0x7c0.
constexpr u64 ViewKeyLight = 0x7b0;
// View: the colour behind everything when there is no sky +0x8, inverse(P) +0xe0, the world-space
// direction the key light's light travels in +0x7d0, the list of skies +0xc80 with its count
// +0xc88, and the rotation from world space to the sky's space +0xcd0.
constexpr u64 ViewBackground = 0x8;
constexpr u64 ViewInverseProjection = 0xe0;
constexpr u64 ViewSunTravel = 0x7d0;
constexpr u64 ViewSkies = 0xc80;
constexpr u64 ViewSkyCount = 0xc88;
constexpr u64 ViewSkyRotation = 0xcd0;
// View: the sun's angle and soft edge in degrees +0xad0 and +0xad4, its colour +0xad8; the clouds'
// scale +0xae4, the second layer's added scale +0xae8, the size their scroll wraps at +0xaec, their
// texture scale over that size +0xaf0 and their opacity +0xaf8.
constexpr u64 ViewSunSize = 0xad0;
constexpr u64 ViewSunOuter = 0xad4;
constexpr u64 ViewSunColor = 0xad8;
constexpr u64 ViewCloudScale = 0xae4;
constexpr u64 ViewCloudScaleStep = 0xae8;
constexpr u64 ViewCloudSize = 0xaec;
constexpr u64 ViewCloudTexcoord = 0xaf0;
constexpr u64 ViewCloudOpacity = 0xaf8;
constexpr float DegreesToRadians = 0.017453292f;
// A sky: the transform of its cube map +0, the texture objects of its cube map +0x40 and clouds
// +0x50, its weight +0x58. The frame graph keeps the clouds' scroll at +0x2fb8, which the frame
// draws after the capture moves on by one frame's wind.
constexpr u64 SkyBytes = 0x70;
constexpr u64 SkyCube = 0x40;
constexpr u64 SkyCloud = 0x50;
constexpr u64 SkyWeight = 0x58;
constexpr u64 FrameGraphCloudScroll = 0x2fb8;
// View: the exposure {stops, adaption range, adaption rate, key value} +0x8a0, the wobble's amount
// +0x8b0, the lens +0x20c, the time +0x1078 (a double), each channel's curve +0xa00 and the curve
// all channels add +0xa54 (7 floats each), the saturation +0xa70.
constexpr u64 ViewExposure = 0x8a0;
constexpr u64 ViewFluctuation = 0x8b0;
constexpr u64 ViewLens = 0x20c;
constexpr u64 ViewTime = 0x1078;
constexpr u64 ViewCurves = 0xa00;
constexpr u64 ViewSaturation = 0xa70;
// View: the bloom's bright pass levels +0xa74 and +0xa84 (4 floats each), the power of its colour
// +0xa94, the power of depth and the near and far scales +0xa98, the depth fades +0xaa4, the blur's
// width range +0xab4, the share of the last frame +0xabc, the colour +0xac0 and strength +0xacc.
constexpr u64 ViewBloomLow = 0xa74;
constexpr u64 ViewBloomHigh = 0xa84;
constexpr u64 ViewBloomPower = 0xa94;
constexpr u64 ViewBloomCorrection = 0xa98;
constexpr u64 ViewBloomDepths = 0xaa4;
constexpr u64 ViewBloomWidth = 0xab4;
constexpr u64 ViewBloomFeedback = 0xabc;
constexpr u64 ViewBloomColor = 0xac0;
constexpr u64 ViewBloomStrength = 0xacc;
// View, for the fog: the eye in the sky's rotated space +0x1b0; the last frame's time step +0x1084;
// the sun's direction in view space +0x7c0 and in world space +0x7d0; the projection's scales +0xa0
// and +0xb4; the near plane +0x120 and the fog's distance +0x8cc; the sun's strength +0x8b4, the
// clouds' brightness +0x8b8, the phase asymmetries +0x8bc and +0x8c0, the least extinction +0x8c4
// and its density +0x8c8, the noise's frequency, scale and attenuation +0x8d0..; the cumulus
// layers' bases +0x8e8, heights +0x8f4, densities +0x900 and scales +0x918 (3 floats each); the
// darkness scale +0x924; the shadow and cloud shadow +0x928, +0x92c; the stratus map +0x930.. and
// its bake +0x940..; the region map +0x968.. and its bake +0x978..; the height fog +0x9a0; the
// colours +0x9a8 and +0x9b8; the sky's rotation +0xcd0 and the fog's space +0xd10.
constexpr u64 ViewRotatedEye = 0x1b0;
constexpr u64 ViewTimeStep = 0x1084;
constexpr u64 ViewSunView = 0x7c0;
constexpr u64 ViewFogDistance = 0x8cc;
constexpr u64 ViewFogLight = 0x8b4;
constexpr u64 ViewFogCloud = 0x8b8;
constexpr u64 ViewFogPhaseAir = 0x8bc;
constexpr u64 ViewFogPhaseCloud = 0x8c0;
constexpr u64 ViewFogExtinction = 0x8c4;
constexpr u64 ViewFogExtinctionDensity = 0x8c8;
constexpr u64 ViewFogNoise = 0x8d0;
constexpr u64 ViewCumulusBase = 0x8e8;
constexpr u64 ViewCumulusHeight = 0x8f4;
constexpr u64 ViewCumulusDensity = 0x900;
constexpr u64 ViewCumulusScale = 0x918;
constexpr u64 ViewFogDarkness = 0x924;
constexpr u64 ViewFogShadow = 0x928;
constexpr u64 ViewFogCloudShadow = 0x92c;
constexpr u64 ViewStratus = 0x930;
constexpr u64 ViewStratusBake = 0x940;
constexpr u64 ViewStratusOffset = 0x95c;
constexpr u64 ViewRegion = 0x968;
constexpr u64 ViewRegionBake = 0x978;
constexpr u64 ViewRegionOffset = 0x994;
constexpr u64 ViewPuddle = 0x9a0;
constexpr u64 ViewFogColor = 0x9a8;
constexpr u64 ViewFogLightColor = 0x9b8;
constexpr u64 ViewFogSpace = 0xd10;
// The frame graph: the noise volumes' texture records +0x3e48 (Perlin) and +0x3f00 (fbm), whose
// +0x28 points at a pointer to their bytes; the noise's offset +0x42cc, the cumulus layers' x and z
// offsets +0x42d8 and +0x42e4, the stratus and region maps' +0x42f0 and +0x42f8; the object at
// +0x3018 holds the cumulus texture objects at +0x91e0.
constexpr std::array<u64, 2> FrameGraphFogNoise{0x3e48, 0x3f00};
constexpr u64 TextureRecordMemory = 0x28;
constexpr u64 FrameGraphFogScroll = 0x42cc;
constexpr u64 FrameGraphCumulusOffsets = 0x42d8;
constexpr u64 FrameGraphLayerOffsets = 0x42f0;
constexpr u64 FrameGraphFogTextures = 0x3018;
constexpr u64 FogCumulusTextures = 0x91e0;
constexpr u32 FogCumulusCount = 4;
// View: the last frame's view-projection +0x320, the distance the world's lines measure depth
// against +0x124, the projection terms of a depth +0x820, the cascades' split depths +0x730, where
// w is the distance shadows end at; the highlights' fade +0x830; the occluder direction +0x7f0; the
// contour's colour +0x800, its blend near and past its fade +0x838 and +0x83c, and the fade's
// distances +0x840 and +0x844; the hatching's strength +0x84c; the toning +0x888, the ambient
// strength +0x88c; the ambient occlusion's strength +0x894 and the value that picks its program
// +0x89c; the gradation's distances +0x9c4 and +0x9c8 and colours +0x9cc (three of 3
// floats); the background's distances +0x9f0 and +0x9f4, strength +0x9f8 and level +0x9fc; the
// occluders +0xc40 with their count +0xc48 and a mode +0xc4c; the ambient light's list +0xc60 with
// its count +0xc68.
constexpr u64 ViewProjectionTerms = 0x820;
constexpr u64 ViewPreviousViewProjection = 0x320;
constexpr u64 ViewDepthRange = 0x124;
constexpr u64 ViewSplitDepths = 0x730;
constexpr u64 ViewShadowEnd = 0x73c;
constexpr u64 ViewSpecularFade = 0x830;
constexpr u64 ViewOccluderDirection = 0x7f0;
constexpr u64 ViewContourColor = 0x800;
constexpr u64 ViewContourBlend = 0x838;
constexpr u64 ViewContourDistantBlend = 0x83c;
constexpr u64 ViewContourStart = 0x840;
constexpr u64 ViewContourEnd = 0x844;
constexpr u64 ViewHatching = 0x84c;
constexpr u64 ViewToning = 0x888;
constexpr u64 ViewAmbient = 0x88c;
constexpr u64 ViewOcclusion = 0x894;
constexpr u64 ViewOcclusionSelect = 0x89c;
constexpr u64 ViewGradationStart = 0x9c4;
constexpr u64 ViewGradationEnd = 0x9c8;
constexpr u64 ViewGradation = 0x9cc;
constexpr u64 ViewBackgroundStart = 0x9f0;
constexpr u64 ViewBackgroundEnd = 0x9f4;
constexpr u64 ViewBackgroundIntensity = 0x9f8;
constexpr u64 ViewBackgroundLod = 0x9fc;
constexpr u64 ViewOccluders = 0xc40;
constexpr u64 ViewOccluderCount = 0xc48;
constexpr u64 ViewOccluderMode = 0xc4c;
constexpr u64 ViewAmbientLights = 0xc60;
constexpr u64 ViewAmbientLightCount = 0xc68;
// An ambient light: the texture objects of its radiance +0xb0 and irradiance +0xb8 cube maps, its
// weight +0xc4.
constexpr u64 AmbientLightBytes = 0xd0;
constexpr u64 AmbientLightRadiance = 0xb0;
constexpr u64 AmbientLightIrradiance = 0xb8;
constexpr u64 AmbientLightWeight = 0xc4;
// Each cascade record holds world space to its texture coordinates and depth at +0xd0.
constexpr u64 CascadeTexture = 0xd0;
constexpr float MinFadeDistance = 0.0001f;
constexpr float ShadowEndFade = 10.0f;
// The occluders' strength, penumbra and range, and the same in the view's other mode.
constexpr std::array<float, 3> OccluderConstants{0.65f, 5.0f, 1.25f};
constexpr std::array<float, 3> OccluderModeConstants{0.3f, 14.0f, 2.2222223f};
// The frame graph holds the hatching texture object at +0x91b8, and at +0x1f20 the ambient light's
// spherical harmonics, which the frame draws after the capture sets from the view's ambient lights.
constexpr u64 FrameGraphHatching = 0x91b8;
constexpr u64 FrameGraphSh = 0x1f20;
// A G2_BG_Main item: material flags +0x30, the texture objects of its layered colour +0x40, normal
// map +0x48, layered normal map +0x50 and mask +0x58. Its vertex constants hold the gradation at
// +0xc0; its pixel constants, the head of a Scene::Material, take 0xa0 bytes.
constexpr u64 MainFlags = 0x30;
constexpr u64 MainLayeredColor = 0x40;
constexpr u64 MainNormalMap = 0x48;
constexpr u64 MainLayeredNormal = 0x50;
constexpr u64 MainMask = 0x58;
constexpr u64 MainGradation = 0xc0;
constexpr u64 MainPixelConstantBytes = 0xa0;
constexpr u64 MainPunchThrough = 0x70;
constexpr u32 MainFlagAlpha = 0x2;
constexpr u32 MainFlagCull = 0x4;
constexpr u32 MainFlagNoHatching = 0x10;
constexpr u32 MainFlagGradation = 0x40;
// A G2_BG_Layer item: flags and gradation as G2_BG_Main's, the texture objects of its four colour
// maps from +0x48 and of its four normal maps from +0x68, pixel constants as
// Scene::LayerMaterial's.
constexpr u64 LayerColorMaps = 0x48;
constexpr u64 LayerNormalMaps = 0x68;
constexpr u64 LayerPunchThrough = 0x134;
// A G2_CH_Main item: the texture objects of its normal map +0x38, reflection +0x40 and detail
// +0x48. Its vertex constants hold the normal bend's point at +0x140 and its range and bias at
// +0x1c8.
constexpr u64 CharacterNormalMap = 0x38;
constexpr u64 CharacterReflection = 0x40;
constexpr u64 CharacterDetail = 0x48;
constexpr u64 CharacterBendPosition = 0x140;
constexpr u64 CharacterBend = 0x1c8;
// The game's Composite pass takes these from a view's values.
constexpr u32 CurveKnots = 7;
constexpr float LensOutwardScale = 1.3f;
constexpr float LensOutwardShape = 1.4281480f; // tan(55 degrees)
constexpr float LensInwardBend = 6.0f;
// The game's adaption rate counts seconds at 30 frames per second.
constexpr float AdaptionFrame = 1.0f / 30.0f;
// The game moves a bloom level that equals its start up by this, and so the blur's width range.
constexpr float BloomLevelStep = 1.0f / 255.0f;
constexpr float BloomWidthStep = 0.0001f;
// The game's SSAO scales the view's strength by this (FUN_0142ec00).
constexpr float OcclusionScale = 0.0863267f;
// The fog's jitter clocks run on the time step and start again past this.
constexpr float FogClockWrap = 10.0f;
constexpr u64 CasterTableBytes = 0x18;
// Shadow caster items: flags +4 (0x08 culls back faces, 0x10 and 0x20 pick a view-projection
// table), cascade mask +5, world matrix +0x30. Alpha-tested casters hold pixel constants {alpha
// reference or -1, fade or -1} and a colour texture object. Each category keeps an index range
// that overrides the submesh's.
constexpr u64 CasterFlags = 0x4;
constexpr u64 CasterMask = 0x5;
constexpr u64 CasterWorld = 0x30;
// A field a category does not have.
constexpr u64 None = ~0ull;
struct CasterCategory {
    u32 index;
    u64 range;
    u64 constants;
    u64 texture;
};
constexpr std::array<CasterCategory, 2> CasterCategories{{
    {0, 0x38, None, None}, // FUN_0141c510
    {2, 0x48, 0x38, 0x40}, // FUN_0141c9b0: alpha-tested
}};
// Item: bone count +0x06, submesh +0x08, vertex buffer object +0x10 (the shape's, or a deformed
// copy the game's compute writes each frame), skin vertex buffer object +0x18 (in the shape's
// skin object), index buffer object +0x20 (at shape + 0x188), bone palette +0x28 (3x4 matrices).
constexpr u64 ItemBones = 0x06;
constexpr u64 ItemSubmesh = 0x08;
constexpr u64 ItemVertexBuffer = 0x10;
constexpr u64 ItemSkin = 0x18;
constexpr u64 ItemIndexBuffer = 0x20;
constexpr u64 ItemPalette = 0x28;
constexpr u64 ShapeVertexBuffer = 0x60;
constexpr u64 ShapeIndexBuffer = 0x188;
constexpr u64 ShapeSkin = 0x1a8;
constexpr u64 SkinVertexBuffer = 0x68;
constexpr u64 BoneBytes = 48;
// Submesh: first index in the shape's indices +0x20, range count +0x1e, first range +0x40.
constexpr u64 SubmeshFirstIndex = 0x20;
constexpr u64 SubmeshRanges = 0x1e;
constexpr u64 SubmeshRange = 0x40;

// The opaque mesh categories. Each item holds its vertex constants, which start with WorldView
// and WorldViewProj, its colour texture object and its pixel constants; the pixel constants hold
// the colour scale and the colour texture coordinate transform (offset xy, repeat zw).
struct Category {
    u32 index;
    u64 constants;
    u64 texture;
    u64 material;
    u64 color_scale;
    u64 texcoord_transform;
    u64 decal = None;
    bool main_material = false;
    bool character = false;
    bool interior = false;
    bool layers = false;
    bool glass = false;
    bool self_translucent = false;
    bool gem = false;
};
constexpr Category MainCategory{15, 0x60, 0x38, 0x68, 0x00, 0x20, None, true}; // G2_BG_Main
// The world's other opaque categories, drawn into the surfaces after G2_BG_Main.
constexpr std::array<Category, 6> WorldCategories{{
    {16, 0x38, 0x48, 0x40, 0x80, 0x00, None, false, false, false, true}, // BG layer
    {17, 0x38, 0x48, 0x40, 0x00, None},                                  // BG single
    {19, 0x38, 0x48, 0x40, None, None, None, false, false, false, false, false, false,
     true},                             // BG gem
    {21, 0x38, 0x48, 0x40, 0x00, None}, // BG interior
    {22, 0x38, 0x48, 0x40, 0x30, 0x00}, // BG vein
    {23, 0x30, None, None, None, None}, // Far: untextured
}};
// G2_CH_Main: colour texture t1, decal texture t6.
constexpr Category CharacterCategory{8, 0x68, 0x30, 0x70, 0xa0, None, 0x50, false, true};
// G2_CH_Interior: its vertex constants are World and WorldViewProj; its pixel constants take 0xb0
// bytes; the texture objects of its two cube maps are at +0x30 and +0x38.
constexpr Category InteriorCategory{13, 0x40, None, 0x48, None, None, None, false, false, true};
constexpr u64 InteriorCube0 = 0x30;
constexpr u64 InteriorCube1 = 0x38;
constexpr u64 InteriorPixelBytes = 0xb0;
// G2_BG_Gem: the texture objects of its colour and normal maps, and its pixel constants, whose
// last word is the screen door's threshold.
constexpr u64 GemColorMap = 0x48;
constexpr u64 GemNormalMap = 0x50;
constexpr u64 GemPixelBytes = 0x60;
constexpr u64 GemPunchThrough = 0x50;
// G2_CH_Contour: the constants of its vertex, geometry and pixel programs. The submesh holds the
// range of its adjacency indices at +0x50, when it has more than 2 ranges.
// G2_BG_Glass: normal map, and the pixel constants whole.
constexpr Category GlassCategory{18,   0x38,  0x48,  0x40,  None,  None,
                                 None, false, false, false, false, true};
// SelfTranslucent: its vertex constants are WorldViewProj and the distance to move the skin along
// its normal; its pixel constants are one colour; the item holds its stencil value.
constexpr Category SelfTranslucentCategory{10,    0x38,  None,  0x40,  0x00,  None, None,
                                           false, false, false, false, false, true};
constexpr u64 SelfTranslucentStencil = 0x30;
// G2_EF_Main: the game draws these items from sorted lists of the view, whose entries hold the
// item's key, a group at +4, an id at +6 and a depth at +8. An item holds the texture objects of
// its displacement map and its colour maps, the constants of its programs, its blend mode and, in
// the top bits of a byte, its depth mode.
constexpr u64 ViewEffectLists = 0xd58;
constexpr u64 ViewEffectListBytes = 0x20;
constexpr u64 EffectEntryBytes = 12;
constexpr u32 EffectCategory = 26;
constexpr u32 ItemKeyCategoryShift = 26;
constexpr u32 ItemKeyOffsetMask = 0x3fffff;
constexpr u32 ItemKeyOffsetShift = 6;
constexpr u64 EffectDisplacement = 0x30;
constexpr u64 EffectColorMaps = 0x38;
constexpr u64 EffectVertexConstants = 0x58;
constexpr u64 EffectPixelConstants = 0x60;
constexpr u64 EffectBlendMode = 0x68;
constexpr u64 EffectDepthMode = 0x6a;
constexpr u32 EffectBlendModes = 8;
constexpr u32 EffectDepthModeShift = 5;
// The pixel constants' word that counts point lights, and those of the four layers' blend
// methods, one of which refracts the scene's copy.
constexpr u32 EffectPointLightsWord = 0x110 / 4;
constexpr u32 EffectBlendMethodWord = 4;
constexpr u32 EffectLayers = 4;
constexpr u32 EffectBackgroundMethod = 16;
// G2_BG_Water: an item holds the texture objects of its displacement map and its other maps, in
// the order of the program's slots, and the constants of its programs. Its draw callback writes the
// count of point lights and the view's water barriers into the pixel constants.
constexpr u32 WaterCategory = 32;
constexpr u64 WaterDisplacement = 0x30;
constexpr std::array<u64, 5> WaterMaps{0x48, 0x50, 0x58, 0x38, 0x40};
constexpr u64 WaterVertexConstants = 0x60;
constexpr u64 WaterPixelConstants = 0x68;
constexpr u32 WaterPointLightsWord = 0xfc / 4;
constexpr u32 WaterBarrierCountWord = 0x1cc / 4;
constexpr u32 WaterBarrierWord = 0x1d0 / 4;
constexpr u64 WaterBarrierBytes = 0x20;
constexpr u32 WaterMaxBarriers = 2;
constexpr u64 ViewWaterBarrierCount = 0x1088;
constexpr u64 ViewWaterBarriers = 0x1090;
// The view's reflection probes: the program takes the cube of the heaviest.
constexpr u64 ViewReflections = 0xc80;
constexpr u64 ViewReflectionCount = 0xc88;
constexpr u64 ReflectionBytes = 0x70;
constexpr u64 ReflectionCube = 0x40;
constexpr u64 ReflectionWeight = 0x58;
// G2_BG_Metaball: an item holds its sphere +0x40 (centre, radius), its vertex constants +0x50 and
// pixel constants +0x58 and the texture object of its displacement map +0x60. Its draw writes into
// the vertex constants up to six neighbours from +0x110, the spheres of the category's other items
// closer than the two radii plus 4, each as its centre and 1 / (radius + 4), and their count at
// +0x170.
constexpr u32 MetaballCategory = 20;
constexpr u64 MetaballSphere = 0x40;
constexpr u64 MetaballVertexConstants = 0x50;
constexpr u64 MetaballPixelConstants = 0x58;
constexpr u64 MetaballDisplacement = 0x60;
constexpr u32 MetaballNeighbourWord = 0x110 / 4;
constexpr u32 MetaballNeighbourCountWord = 0x170 / 4;
constexpr u32 MetaballNeighbours = 6;
constexpr float MetaballReach = 4.0f;
constexpr u32 ContourCategory = 11;
constexpr u64 ContourVertexConstants = 0x30;
constexpr u64 ContourGeometryConstants = 0x38;
constexpr u64 ContourPixelConstants = 0x40;
constexpr u64 SubmeshAdjacencyRange = 0x50;
constexpr u32 ContourMinRanges = 3;
// Branch leaves: the texture object +0x30, the constants of the hull program +0x38 {segments,
// leaves}, of the domain program +0x40 {WVP, the last frame's WVP, World, VP, then direction,
// twist, width, gravity, length} and of the pixel program +0x48 {tip colour, base colour, alpha
// reference, translucency, roughness, metallic, screen door}, and the vertex count +0x50, drawn
// as patches of three. Leaf shadow items, with the casters' mask, keep World and the shape values
// at +0x40 and +0xc0 of their domain constants, and {alpha reference, screen door} as pixel
// constants.
constexpr u32 LeafCategory = 29;
constexpr u32 LeafShadowCategory = 30;
constexpr u64 LeafTexture = 0x30;
constexpr u64 LeafHullConstants = 0x38;
constexpr u64 LeafDomainConstants = 0x40;
constexpr u64 LeafPixelConstants = 0x48;
constexpr u64 LeafVertexCount = 0x50;
constexpr u64 LeafWorld = 0x80;
constexpr u64 LeafShape = 0x100;
constexpr u64 LeafPixelValues = 0x20;
constexpr u64 LeafShadowWorld = 0x40;
constexpr u64 LeafShadowShape = 0xc0;
constexpr u32 LeafShapeValues = 5;
constexpr u32 LeafPixelCount = 5;
// Grass, deformer kind 5: a record holds its shape +0x38, the indirect arguments object it shares
// with its item +0x50 and its kernel's constants +0x58: World and its inverse, the extra force
// +0x100, the vertex count +0x110, the clip range +0x118 and the blade shape from +0x124.
constexpr u32 KindGrass = 5;
constexpr u64 GrassRecordShape = 0x38;
constexpr u64 GrassRecordArguments = 0x50;
constexpr u64 GrassRecordConstants = 0x58;
constexpr u64 GrassExtraForce = 0x100;
constexpr u64 GrassVertexCount = 0x110;
constexpr u64 GrassClipRange = 0x118;
constexpr u64 GrassShapeValues = 0x124;
constexpr u32 GrassShapeValueCount = 7;
constexpr u32 GrassVertexBytes = 32;
constexpr u32 GrassGroupSize = 64;
// The game advances an instance's wind while it moves faster than this.
constexpr float GrassMovingForce = 1.5f;
constexpr float FullTurn = 6.2831855f;
// A grass item: the indirect arguments object +0x30, the texture objects of its colour +0x38 and
// normal map +0x40, the constants of its geometry program +0x48 {WVP, the last frame's WVP, eye
// +0x80, width +0x90} and of its pixel program +0x50 {tip colour, root colour, alpha reference,
// translucency, roughness, metallic, screen door, normal scale}.
constexpr u32 GrassCategory = 31;
constexpr u64 GrassArguments = 0x30;
constexpr u64 GrassColor = 0x38;
constexpr u64 GrassNormal = 0x40;
constexpr u64 GrassGeometryConstants = 0x48;
constexpr u64 GrassPixelConstants = 0x50;
constexpr u64 GrassEye = 0x80;
constexpr u64 GrassWidth = 0x90;
constexpr u64 GrassPixelValues = 0x20;
constexpr u32 GrassPixelCount = 6;
constexpr u32 PatchVertices = 3;
constexpr u32 QuadVertices = 6;
// Branch, deformer kind 4: a record holds its instance's vertex buffer object +0x10, its shape
// +0x38 and its kernel's constants +0x48: World, its inverse and WVP, the sway's cycle +0xc0, the
// clip depth's scale to the effector grid +0xc4 and the vertex count +0xcc.
constexpr u32 KindBranch = 4;
constexpr u64 BranchOutput = 0x10;
constexpr u64 BranchShape = 0x38;
constexpr u64 BranchConstants = 0x48;
constexpr u64 BranchCycle = 0xc0;
constexpr u64 BranchClusterScale = 0xc4;
constexpr u64 BranchVertexCount = 0xcc;
constexpr u32 BranchVertexBytes = 16;
constexpr u32 BranchStateBytes = 12;
constexpr u32 BranchGroupSize = 64;
constexpr u32 PositionHalfFloat = 7;
// A branch ribbon item: the texture, hull and domain constants as a leaf item's. Its hull constants
// start with its segments, its domain constants hold the eye +0x80, then the texture's range, the
// tip and root colours, and the tip and root widths +0xc0. Its pixel constants +0x50 hold {alpha
// reference, translucency, roughness, metallic, screen door}, and its vertex count is +0x58.
constexpr u32 BranchCategory = 28;
constexpr u64 BranchPixelConstants = 0x50;
constexpr u64 BranchItemVertexCount = 0x58;
constexpr u64 BranchEye = 0x80;
constexpr u64 BranchWidths = 0xc0;
constexpr u32 BranchPixelCount = 5;
// The view's decals of the world. A record holds its box to world, its colour and normal maps, the
// scales and texture transforms of both, its material and its parallax depth.
constexpr u64 ViewWorldDecals = 0xc20;
constexpr u64 ViewWorldDecalCount = 0xc28;
constexpr u64 WorldDecalBytes = 0xb0;
constexpr u64 WorldDecalColor = 0x40;
constexpr u64 WorldDecalNormal = 0x48;
constexpr u64 WorldDecalScales = 0x50;
constexpr u64 WorldDecalRoughness = 0x90;
constexpr u64 WorldDecalMetallic = 0x94;
constexpr u64 WorldDecalParallax = 0x98;
constexpr u64 WorldDecalTranslucency = 0x9c;
// The game's Decal pass scales a record's parallax depth by this.
constexpr float WorldDecalParallaxScale = 0.3f;
// CH_Main's vertex constants hold the decals' positions and targets, then their scales, which are
// 0 when a material has no decals. Its pixel constants hold GradationColor and the alpha step flag.
constexpr u64 DecalPositions = 0x150;
constexpr u64 DecalScales = 0x1b0;
constexpr u64 DecalConstantBytes =
    sizeof(Scene::Decal::positions) + sizeof(Scene::Decal::targets) + sizeof(Scene::Decal::scales);
constexpr u64 GradationColor = 0xb0;
constexpr u64 AlphaStepFlag = 0x1fc;
constexpr std::array<float, 4> NoScale{1.0f, 1.0f, 1.0f, 1.0f};
constexpr std::array<float, 4> NoTransform{0.0f, 0.0f, 1.0f, 1.0f};
constexpr u32 BatchedCategory = 15;
constexpr u64 MatrixBytes = 64;

std::atomic<u64> g_frames{0};
// Whether frames are captured.
std::atomic<bool> g_capturing{true};
// The game's frame rate since the last report, in whichever state the switches are: the frames, the
// time the count began, the last frame's time, the longest gap and the gaps over 50 ms.
constexpr auto FrameRatePeriod = std::chrono::seconds{2};
constexpr auto LongGap = std::chrono::milliseconds{50};
u32 g_rate_frames = 0;
u32 g_rate_long_gaps = 0;
std::chrono::steady_clock::time_point g_rate_start;
std::chrono::steady_clock::time_point g_rate_last;
std::chrono::steady_clock::duration g_rate_longest{};
// When the last frame's capture began.
std::chrono::steady_clock::time_point g_last_frame_start;

// The frame's commands, which reach the packet in one go. Written item by item, they would leave
// half-full write-combined lines in several places for the reads between items to evict.
alignas(64) std::array<std::array<Scene::Command, Scene::MaxDraws>,
                       static_cast<u32>(Scene::DrawList::Count)> g_commands;
alignas(64) std::array<Scene::Command,
                       Scene::Cascades * Scene::CasterLists * Scene::MaxCasters> g_caster_commands;
alignas(64) std::array<Scene::Command, Scene::MaxContours> g_contour_commands;
alignas(64) std::array<Scene::VertexCommand, Scene::MaxLeaves> g_leaf_commands;
alignas(
    64) std::array<Scene::VertexCommand, Scene::Cascades * Scene::MaxLeaves> g_leaf_shadow_commands;
alignas(64) std::array<Scene::VertexCommand, Scene::MaxBranches> g_branch_commands;
alignas(64) std::array<Scene::Command, Scene::MaxEffectDraws> g_effect_commands;
alignas(64) std::array<Scene::EffectRun, Scene::MaxEffectDraws> g_effect_runs;
alignas(64) std::array<Scene::Command, Scene::MaxWaterDraws> g_water_commands;
// The unskinned metaball draws, then the skinned ones.
alignas(64) std::array<std::array<Scene::Command, Scene::MaxMetaballs>, 2> g_metaball_commands;

// A metaball item and its sphere, which its neighbours' draws read.
struct MetaballSphereOf {
    VAddr item;
    std::array<float, 4> sphere;
};
// The wind's place in its cycle. The game keeps one per instance; only its rate shows, and only on
// instances that move fast.
float g_grass_phase = 0.0f;

// The simulate constants the game's SoftBody pass sets on its draw thread after the capture: the
// grid matrix (the view's clip matrix with z over GridFar + n), the sphere count and the depth
// fade.
struct GridConstants {
    std::array<float, 16> matrix;
    u32 spheres;
    std::array<float, 2> fade;
};

// What the game's Decal pass multiplies a decal's box by.
struct DecalView {
    Matrix view;
    Matrix inverse_view;
    Matrix projection;
};

// The box's x and y, from -1 to 1, to the decal's: x from 1 to 0 and y from 0 to -1.
constexpr Matrix BoxToDecal{-0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -0.5f, 0.0f, 0.0f,
                            0.0f,  0.0f, 1.0f, 0.0f, 0.5f, -0.5f, 0.0f, 1.0f};

// What the game's grass pass sets on its draw thread after the capture: the view and clip matrices
// that a grass instance's constants multiply, and the clip depth's scale to the effector grid.
struct GrassView {
    Matrix view;
    Matrix view_projection;
    float cluster_scale;
    float time_step;
};

// Writes the draws of a packet. The packet's memory is write-combined, where reads are slow:
// the writer only stores to it, and counts in its own members.
class Writer {
public:
    /// packet is at address on the GPU; frame is the capture's, which began at start.
    Writer(u8* packet, u64 address, u64 frame, std::chrono::steady_clock::time_point start)
        : frame{frame}, part_start{start}, packet{packet},
          draws{reinterpret_cast<Scene::Draw*>(packet + Scene::DrawsOffset)},
          casters{reinterpret_cast<Scene::Caster*>(packet + Scene::CastersOffset)},
          palettes{packet + Scene::PalettesOffset},
          palettes_address{address + Scene::PalettesOffset},
          skin_jobs{reinterpret_cast<Scene::SkinJob*>(packet + Scene::SkinJobsOffset)},
          soft_body_jobs{reinterpret_cast<Scene::SoftBodyJob*>(packet + Scene::SoftBodyJobsOffset)},
          decals{reinterpret_cast<Scene::Decal*>(packet + Scene::DecalsOffset)},
          materials{reinterpret_cast<Scene::Material*>(packet + Scene::MaterialsOffset)},
          characters{reinterpret_cast<Scene::Character*>(packet + Scene::CharactersOffset)},
          contours{reinterpret_cast<Scene::Contour*>(packet + Scene::ContoursOffset)},
          leaves{reinterpret_cast<Scene::Leaf*>(packet + Scene::LeavesOffset)},
          layer_materials{
              reinterpret_cast<Scene::LayerMaterial*>(packet + Scene::LayerMaterialsOffset)},
          grass{reinterpret_cast<Scene::Grass*>(packet + Scene::GrassOffset)},
          branch_jobs{reinterpret_cast<Scene::BranchJob*>(packet + Scene::BranchJobsOffset)},
          branches{reinterpret_cast<Scene::Branch*>(packet + Scene::BranchesOffset)},
          world_decals{reinterpret_cast<Scene::WorldDecal*>(packet + Scene::WorldDecalsOffset)},
          glass_materials{
              reinterpret_cast<Scene::GlassMaterial*>(packet + Scene::GlassMaterialsOffset)} {}

    // Ends a part of the capture's time.
    void EndPart(Scene::CapturePart part) {
        const auto now = std::chrono::steady_clock::now();
        header.capture_parts_ns[static_cast<u32>(part)] = static_cast<u32>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - part_start).count());
        part_start = now;
    }

    // Writes the frame's commands to the packet.
    void CopyCommands() {
        constexpr u64 Bytes = sizeof(Scene::Command);
        std::memcpy(packet + Scene::WaterCommandsOffset, g_water_commands.data(),
                    header.effects.water_draws * Bytes);
        u8* out = packet + Scene::CommandsOffset;
        for (u32 list = 0; list < g_commands.size(); list++) {
            const u64 bytes = header.draw_commands[list] * Bytes;
            std::memcpy(out, g_commands[list].data(), bytes);
            out += bytes;
        }
        for (u32 c = 0; c < Scene::Cascades; c++) {
            for (u32 list = 0; list < Scene::CasterLists; list++) {
                const u32 first = (c * Scene::CasterLists + list) * Scene::MaxCasters;
                std::memcpy(packet + Scene::CasterCommandsOffset + first * Bytes,
                            g_caster_commands.data() + first,
                            header.caster_commands[c][list] * Bytes);
            }
        }
        std::memcpy(packet + Scene::ContourCommandsOffset, g_contour_commands.data(),
                    header.contours * Bytes);
        constexpr u64 VertexBytes = sizeof(Scene::VertexCommand);
        std::memcpy(packet + Scene::LeafCommandsOffset, g_leaf_commands.data(),
                    header.leaf_commands * VertexBytes);
        for (u32 c = 0; c < Scene::Cascades; c++) {
            std::memcpy(packet + Scene::LeafShadowCommandsOffset +
                            c * Scene::MaxLeaves * VertexBytes,
                        g_leaf_shadow_commands.data() + c * Scene::MaxLeaves,
                        header.leaf_shadow_commands[c] * VertexBytes);
        }
        if (header.grass != 0) {
            const Scene::VertexCommand grass_command{.instance_count = 1};
            std::memcpy(packet + Scene::GrassCommandOffset, &grass_command, sizeof(grass_command));
        }
        std::memcpy(packet + Scene::BranchCommandsOffset, g_branch_commands.data(),
                    header.branches * VertexBytes);
        for (u32 list = 0; list < g_metaball_commands.size(); list++) {
            std::memcpy(packet + Scene::MetaballCommandsOffset +
                            u64{list} * Scene::MaxMetaballs * Bytes,
                        g_metaball_commands[list].data(), header.metaball_commands[list] * Bytes);
        }
    }

    void SetCascades(u32 mask) {
        header.cascades = mask;
    }

    void SetSpheres(u32 count) {
        header.spheres = count;
    }

    void SetHatching(u32 handle) {
        hatching = handle;
    }

    void SetSky(u32 cube, const std::array<float, 3>& background) {
        header.sky = cube;
        std::memcpy(header.background, background.data(), sizeof(header.background));
    }

    void SetDepthBias(u32 cascade, float constant, float slope) {
        header.depth_bias[cascade][0] = constant;
        header.depth_bias[cascade][1] = slope;
    }

    const Scene::Header& Header() const {
        return header;
    }

    void Add(VAddr item, const Category& category) {
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr constants = Load<VAddr>(item + category.constants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || constants == 0 ||
            Load<u16>(submesh + SubmeshRanges) == 0 || header.draws == Scene::MaxDraws) {
            header.dropped++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr) {
            header.dropped++;
            return;
        }
        const u32 first =
            Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
        const u32 count = Load<u32>(submesh + SubmeshRange + 4);
        Skin skin;
        u64 vertices = mesh->vertices;
        if (u64{first} + count > mesh->indices) {
            header.dropped++;
            return;
        }
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (vertex_buffer != shape + ShapeVertexBuffer) {
            // The game's compute has deformed the vertices; the renderer's copy holds its own.
            vertices = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
            if (vertices == 0) {
                header.deformed_waiting++;
                return;
            }
            header.deformed++;
        } else if (!ReadSkin(item, shape, *mesh, skin)) {
            header.dropped++;
            return;
        }
        header.indices += count;
        header.skinned += skin.bones != 0;
        header.max_indices = std::max(header.max_indices, count);
        u32 texture = 0;
        if (category.texture != None) {
            const VAddr object = Load<VAddr>(item + category.texture);
            if (object != 0) {
                texture = Textures::Get(object);
                header.untextured += texture == 0;
            }
        }
        const VAddr material =
            category.material != None ? Load<VAddr>(item + category.material) : 0;
        const void* color_scale = NoScale.data();
        const void* texcoord_transform = NoTransform.data();
        if (material != 0 && category.color_scale != None) {
            color_scale = reinterpret_cast<const void*>(material + category.color_scale);
        }
        if (material != 0 && category.texcoord_transform != None) {
            texcoord_transform =
                reinterpret_cast<const void*>(material + category.texcoord_transform);
        }
        u32 decal = 0;
        if (material != 0 && category.decal != None) {
            decal = AddDecal(constants, material, Load<VAddr>(item + category.decal));
        }
        u32 material_index = 0;
        bool alpha_tested = false;
        // The game's world passes cull back faces; G2_BG_Main does by the material's flag.
        bool culled = true;
        if (category.main_material && material != 0) {
            const u32 flags = Load<u32>(item + MainFlags);
            culled = (flags & MainFlagCull) != 0;
            alpha_tested =
                (flags & MainFlagAlpha) != 0 || Load<float>(material + MainPunchThrough) < 1.0f;
            material_index = AddMainMaterial(item, flags, constants, material, *mesh);
        }
        if (category.gem && material != 0) {
            material_index = AddGem(item, material);
            alpha_tested = material_index != 0 && Load<float>(material + GemPunchThrough) < 1.0f;
        }
        if (category.layers && material != 0) {
            material_index = AddLayers(item, constants, material);
            alpha_tested = material_index != 0 && Load<float>(material + LayerPunchThrough) < 1.0f;
        }
        if (category.character) {
            if (material == 0 || header.characters == Scene::MaxCharacters) {
                header.dropped_characters += material != 0;
                header.dropped++;
                return;
            }
            material_index = AddCharacter(item, constants, material);
        }
        if (category.interior) {
            if (material == 0 || header.characters == Scene::MaxCharacters) {
                header.dropped_characters += material != 0;
                header.dropped++;
                return;
            }
            material_index = AddInterior(item, material);
        }
        if (category.glass) {
            if (material == 0 || mesh->normal == 0 || mesh->tangent == 0 ||
                header.glass_materials == Scene::MaxGlassMaterials) {
                header.dropped_glass++;
                header.dropped++;
                return;
            }
            std::memcpy(&glass_materials[header.glass_materials],
                        reinterpret_cast<const void*>(material), sizeof(Scene::GlassMaterial));
            material_index = ++header.glass_materials;
        }
        u32 stencil = 0;
        if (category.self_translucent) {
            stencil = Load<u8>(item + SelfTranslucentStencil);
            if (material == 0 || stencil == 0 || stencil > Scene::SelfTranslucentLists) {
                header.dropped++;
                return;
            }
        }
        const Scene::DrawList list =
            category.self_translucent
                ? static_cast<Scene::DrawList>(static_cast<u32>(Scene::DrawList::SelfTranslucent1) +
                                               stencil - 1)
            : category.glass     ? Scene::DrawList::Glass
            : category.interior  ? Scene::DrawList::Interior
            : category.character ? Scene::DrawList::Character
            : alpha_tested       ? (culled ? Scene::DrawList::AlphaCulled : Scene::DrawList::Alpha)
                           : (culled ? Scene::DrawList::OpaqueCulled : Scene::DrawList::Opaque);
        const u32 i = header.draws++;
        // The fields are written in order: the packet is write-combined.
        Scene::Draw& draw = draws[i];
        std::memcpy(draw.world_view, reinterpret_cast<const void*>(constants), 2 * MatrixBytes);
        std::memcpy(draw.color_scale, color_scale, sizeof(draw.color_scale));
        std::memcpy(draw.texcoord_transform, texcoord_transform, sizeof(draw.texcoord_transform));
        draw.vertices = vertices;
        draw.stride = mesh->stride;
        draw.position = mesh->position;
        draw.texcoord = mesh->texcoord;
        draw.texture = texture;
        draw.skin = skin.stream;
        draw.palette = skin.palette;
        draw.bones = skin.bones;
        draw.decal = decal;
        draw.normal = mesh->normal;
        draw.tangent = mesh->tangent;
        draw.vertex_color = mesh->vertex_color;
        draw.material = material_index;
        u32& commands = header.draw_commands[static_cast<u32>(list)];
        g_commands[static_cast<u32>(list)][commands++] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = i,
        };
    }

    // A G2_CH_Contour item: the mesh's adjacency indices with the item's constants.
    void AddContour(VAddr item) {
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr vertex_constants = Load<VAddr>(item + ContourVertexConstants);
        const VAddr geometry_constants = Load<VAddr>(item + ContourGeometryConstants);
        const VAddr pixel_constants = Load<VAddr>(item + ContourPixelConstants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || vertex_constants == 0 || geometry_constants == 0 ||
            pixel_constants == 0 || Load<u16>(submesh + SubmeshRanges) < ContourMinRanges ||
            header.contours == Scene::MaxContours) {
            header.dropped_contours++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr || mesh->normal == 0) {
            header.dropped_contours++;
            return;
        }
        const u32 first =
            Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshAdjacencyRange);
        const u32 count = Load<u32>(submesh + SubmeshAdjacencyRange + 4);
        Skin skin;
        u64 vertices = mesh->vertices;
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (u64{first} + count > mesh->indices) {
            header.dropped_contours++;
            return;
        }
        if (vertex_buffer != shape + ShapeVertexBuffer) {
            vertices = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
            if (vertices == 0) {
                header.dropped_contours++;
                return;
            }
        } else if (!ReadSkin(item, shape, *mesh, skin)) {
            header.dropped_contours++;
            return;
        }
        const u32 i = header.contours++;
        // The fields are written in order: the packet is write-combined.
        Scene::Contour& contour = contours[i];
        std::memcpy(contour.vertex_constants, reinterpret_cast<const void*>(vertex_constants),
                    sizeof(contour.vertex_constants));
        std::memcpy(contour.geometry_constants, reinterpret_cast<const void*>(geometry_constants),
                    sizeof(contour.geometry_constants));
        std::memcpy(contour.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    sizeof(contour.pixel_constants));
        contour.vertices = vertices;
        contour.stride = mesh->stride;
        contour.position = mesh->position;
        contour.normal = mesh->normal;
        contour.bones = skin.bones;
        contour.skin = skin.stream;
        contour.palette = skin.palette;
        g_contour_commands[i] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = 0,
        };
    }

    // The items of the game's sorted lists, in the order it draws them: by id, group and falling
    // depth, the entries of an id keeping to the least depth so far, then by falling depth.
    void AddEffects(VAddr view) {
        struct Entry {
            u32 key;
            u32 group;
            u32 id;
            u32 depth;
        };
        std::array<Entry, Scene::MaxEffectDraws> entries;
        const VAddr items = Load<VAddr>(view + ViewItems);
        for (u32 list = 0; list < Scene::EffectLists; list++) {
            const VAddr bounds = view + ViewEffectLists + list * ViewEffectListBytes;
            const VAddr begin = Load<VAddr>(bounds);
            const VAddr end = Load<VAddr>(bounds + 8);
            const u64 listed = end > begin ? (end - begin) / EffectEntryBytes : 0;
            const u32 count = static_cast<u32>(std::min<u64>(listed, entries.size()));
            header.effects.dropped += static_cast<u32>(listed - count);
            for (u32 i = 0; i < count; i++) {
                const VAddr entry = begin + i * EffectEntryBytes;
                entries[i] = {
                    .key = Load<u32>(entry),
                    .group = Load<u8>(entry + 4),
                    .id = Load<u16>(entry + 6),
                    .depth = Load<u16>(entry + 8),
                };
            }
            std::sort(entries.begin(), entries.begin() + count, [](const Entry& a, const Entry& b) {
                if (a.id != b.id) {
                    return a.id < b.id;
                }
                if (a.group != b.group) {
                    return a.group < b.group;
                }
                return a.depth > b.depth;
            });
            for (u32 i = 1; i < count; i++) {
                if (entries[i].id == entries[i - 1].id) {
                    entries[i].depth = std::min(entries[i].depth, entries[i - 1].depth);
                }
            }
            for (u32 i = 1; i < count; i++) {
                const Entry entry = entries[i];
                u32 at = i;
                for (; at > 0 && entries[at - 1].depth < entry.depth; at--) {
                    entries[at] = entries[at - 1];
                }
                entries[at] = entry;
            }
            for (u32 i = 0; i < count; i++) {
                if (entries[i].key >> ItemKeyCategoryShift != EffectCategory) {
                    header.effects.unported++;
                    continue;
                }
                const u64 at = static_cast<u64>(entries[i].key & ItemKeyOffsetMask)
                               << ItemKeyOffsetShift;
                AddEffect(items + at, list);
            }
        }
        std::memcpy(packet + Scene::EffectCommandsOffset, g_effect_commands.data(),
                    header.effects.draws * sizeof(Scene::Command));
        std::memcpy(packet + Scene::EffectRunsOffset, g_effect_runs.data(),
                    header.effects.runs * sizeof(Scene::EffectRun));
    }

    // The cube of the view's heaviest reflection probe, or 0.
    static u32 ReflectionCubeOf(VAddr view) {
        const VAddr probes = Load<VAddr>(view + ViewReflections);
        const s32 count = Load<s32>(view + ViewReflectionCount);
        VAddr heaviest = 0;
        for (s32 i = 0; i < count; i++) {
            const VAddr probe = probes + i * ReflectionBytes;
            if (Load<VAddr>(probe + ReflectionCube) != 0 &&
                (heaviest == 0 || Load<float>(probe + ReflectionWeight) >
                                      Load<float>(heaviest + ReflectionWeight))) {
                heaviest = probe;
            }
        }
        return heaviest != 0 ? Textures::GetCube(Load<VAddr>(heaviest + ReflectionCube)) : 0;
    }

    // A G2_BG_Water item.
    void AddWater(VAddr item, VAddr view) {
        Scene::EffectHeader& effects = header.effects;
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr vertex_constants = Load<VAddr>(item + WaterVertexConstants);
        const VAddr pixel_constants = Load<VAddr>(item + WaterPixelConstants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || vertex_constants == 0 || pixel_constants == 0 ||
            Load<u16>(submesh + SubmeshRanges) == 0 ||
            effects.water_draws == Scene::MaxWaterDraws) {
            effects.dropped_water++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr) {
            effects.dropped_water++;
            return;
        }
        const u32 first =
            Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
        const u32 count = Load<u32>(submesh + SubmeshRange + 4);
        u64 vertices = mesh->vertices;
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (vertex_buffer != shape + ShapeVertexBuffer) {
            vertices = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
        }
        if (u64{first} + count > mesh->indices || vertices == 0) {
            effects.dropped_water++;
            return;
        }
        const auto handle = [](VAddr object) { return object != 0 ? Textures::Get(object) : 0; };
        const u32 i = effects.water_draws++;
        // The fields are written in order: the packet is write-combined.
        Scene::WaterDraw& out =
            reinterpret_cast<Scene::WaterDraw*>(packet + Scene::WaterDrawsOffset)[i];
        std::memcpy(out.vertex_constants, reinterpret_cast<const void*>(vertex_constants),
                    sizeof(out.vertex_constants));
        std::memcpy(out.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    sizeof(out.pixel_constants));
        // Point lights are not ported: the programs see none.
        out.pixel_constants[WaterPointLightsWord] = 0;
        out.pixel_constants[WaterBarrierCountWord] =
            std::min(Load<u32>(view + ViewWaterBarrierCount), WaterMaxBarriers);
        std::memcpy(out.pixel_constants + WaterBarrierWord,
                    reinterpret_cast<const void*>(view + ViewWaterBarriers), WaterBarrierBytes);
        out.mesh.vertices = vertices;
        out.mesh.skin = 0;
        out.mesh.palette = 0;
        out.mesh.stride = mesh->stride;
        out.mesh.position = mesh->position;
        out.mesh.texcoord = mesh->texcoord;
        out.mesh.texcoord1 = mesh->texcoord1;
        out.mesh.normal = mesh->normal;
        out.mesh.tangent = mesh->tangent;
        out.mesh.vertex_color = mesh->vertex_color;
        out.mesh.displacement = handle(Load<VAddr>(item + WaterDisplacement));
        for (u32 t = 0; t < WaterMaps.size(); t++) {
            out.mesh.textures[t] = handle(Load<VAddr>(item + WaterMaps[t]));
        }
        out.mesh.reflection = ReflectionCubeOf(view);
        g_water_commands[i] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = i,
        };
    }

    // A G2_BG_Metaball item, given the spheres of all the frame's metaball items.
    void AddMetaball(VAddr item, std::span<const MetaballSphereOf> spheres) {
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr vertex_constants = Load<VAddr>(item + MetaballVertexConstants);
        const VAddr pixel_constants = Load<VAddr>(item + MetaballPixelConstants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || vertex_constants == 0 || pixel_constants == 0 ||
            Load<u16>(submesh + SubmeshRanges) == 0 || header.metaballs == Scene::MaxMetaballs) {
            header.dropped_metaballs++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr) {
            header.dropped_metaballs++;
            return;
        }
        const u32 first =
            Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
        const u32 count = Load<u32>(submesh + SubmeshRange + 4);
        if (u64{first} + count > mesh->indices) {
            header.dropped_metaballs++;
            return;
        }
        Skin skin;
        u64 vertices = mesh->vertices;
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (vertex_buffer != shape + ShapeVertexBuffer) {
            vertices = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
            if (vertices == 0) {
                header.dropped_metaballs++;
                return;
            }
        } else if (!ReadSkin(item, shape, *mesh, skin)) {
            header.dropped_metaballs++;
            return;
        }
        std::array<u32, sizeof(Scene::MetaballDraw::vertex_constants) / sizeof(u32)> constants;
        std::memcpy(constants.data(), reinterpret_cast<const void*>(vertex_constants),
                    sizeof(constants));
        const auto sphere = Load<std::array<float, 4>>(item + MetaballSphere);
        u32 neighbours = 0;
        for (const MetaballSphereOf& other : spheres) {
            if (neighbours == MetaballNeighbours) {
                break;
            }
            if (other.item == item) {
                continue;
            }
            const float dx = sphere[0] - other.sphere[0];
            const float dy = sphere[1] - other.sphere[1];
            const float dz = sphere[2] - other.sphere[2];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <
                sphere[3] + other.sphere[3] + MetaballReach) {
                const std::array<float, 4> neighbour{other.sphere[0], other.sphere[1],
                                                     other.sphere[2],
                                                     1.0f / (other.sphere[3] + MetaballReach)};
                std::memcpy(constants.data() + MetaballNeighbourWord + neighbours * 4,
                            neighbour.data(), sizeof(neighbour));
                neighbours++;
            }
        }
        constants[MetaballNeighbourCountWord] = neighbours;
        const u32 i = header.metaballs++;
        // The fields are written in order: the packet is write-combined.
        Scene::MetaballDraw& out =
            reinterpret_cast<Scene::MetaballDraw*>(packet + Scene::MetaballDrawsOffset)[i];
        std::memcpy(out.vertex_constants, constants.data(), sizeof(out.vertex_constants));
        std::memcpy(out.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    sizeof(out.pixel_constants));
        const VAddr displacement = Load<VAddr>(item + MetaballDisplacement);
        out.mesh = {
            .vertices = vertices,
            .skin = skin.stream,
            .palette = skin.palette,
            .stride = mesh->stride,
            .position = mesh->position,
            .texcoord = mesh->texcoord,
            .texcoord1 = mesh->texcoord1,
            .normal = mesh->normal,
            .tangent = mesh->tangent,
            .vertex_color = mesh->vertex_color,
            .displacement = displacement != 0 ? Textures::Get(displacement) : 0,
        };
        const u32 list = skin.bones != 0 ? 1 : 0;
        g_metaball_commands[list][header.metaball_commands[list]++] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = i,
        };
    }

    // A G2_EF_Main item of one of the game's lists.
    void AddEffect(VAddr item, u32 list) {
        Scene::EffectHeader& effects = header.effects;
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr vertex_constants = Load<VAddr>(item + EffectVertexConstants);
        const VAddr pixel_constants = Load<VAddr>(item + EffectPixelConstants);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || vertex_constants == 0 || pixel_constants == 0 ||
            Load<u16>(submesh + SubmeshRanges) == 0 || effects.draws == Scene::MaxEffectDraws) {
            effects.dropped++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr) {
            effects.dropped++;
            return;
        }
        const u32 first =
            Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
        const u32 count = Load<u32>(submesh + SubmeshRange + 4);
        if (u64{first} + count > mesh->indices) {
            effects.dropped++;
            return;
        }
        Skin skin;
        u64 vertices = mesh->vertices;
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (vertex_buffer != shape + ShapeVertexBuffer) {
            vertices = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
            if (vertices == 0) {
                effects.dropped++;
                return;
            }
        } else if (!ReadSkin(item, shape, *mesh, skin)) {
            effects.dropped++;
            return;
        }
        const auto handle = [](VAddr object) { return object != 0 ? Textures::Get(object) : 0; };
        const u32 i = effects.draws++;
        // The fields are written in order: the packet is write-combined.
        Scene::EffectDraw& out =
            reinterpret_cast<Scene::EffectDraw*>(packet + Scene::EffectDrawsOffset)[i];
        std::memcpy(out.vertex_constants, reinterpret_cast<const void*>(vertex_constants),
                    sizeof(out.vertex_constants));
        std::memcpy(out.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    sizeof(out.pixel_constants));
        // Point lights are not ported: the programs see none.
        out.pixel_constants[EffectPointLightsWord] = 0;
        out.mesh.vertices = vertices;
        out.mesh.skin = skin.stream;
        out.mesh.palette = skin.palette;
        out.mesh.stride = mesh->stride;
        out.mesh.position = mesh->position;
        out.mesh.texcoord = mesh->texcoord;
        out.mesh.texcoord1 = mesh->texcoord1;
        out.mesh.normal = mesh->normal;
        out.mesh.tangent = mesh->tangent;
        out.mesh.vertex_color = mesh->vertex_color;
        out.mesh.displacement = handle(Load<VAddr>(item + EffectDisplacement));
        for (u32 t = 0; t < 4; t++) {
            out.mesh.textures[t] = handle(Load<VAddr>(item + EffectColorMaps + t * 8));
        }
        g_effect_commands[i] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = i,
        };
        const u32 mode = Load<u16>(item + EffectBlendMode);
        const u32 blend = mode < EffectBlendModes ? mode : 0;
        const u32 depth_mode = Load<u8>(item + EffectDepthMode) >> EffectDepthModeShift;
        const u32 skinned = skin.bones != 0 ? 1 : 0;
        effects.lists |= 1u << list;
        for (u32 layer = 0; layer < EffectLayers; layer++) {
            if (Load<u32>(pixel_constants + (EffectBlendMethodWord + layer) * 4) ==
                EffectBackgroundMethod) {
                effects.background |= 1u << list;
            }
        }
        if (effects.runs != 0) {
            Scene::EffectRun& last = g_effect_runs[effects.runs - 1];
            if (last.list == list && last.blend == blend && last.depth_mode == depth_mode &&
                last.skinned == skinned) {
                last.count++;
                return;
            }
        }
        g_effect_runs[effects.runs++] = {
            .list = list,
            .blend = blend,
            .depth_mode = depth_mode,
            .skinned = skinned,
            .first = i,
            .count = 1,
        };
    }

    // A record of the view's decals: the matrices the game's Decal pass computes, then the record.
    void AddWorldDecal(VAddr record, const DecalView& view) {
        const VAddr color = Load<VAddr>(record + WorldDecalColor);
        const VAddr normal = Load<VAddr>(record + WorldDecalNormal);
        const u32 color_map = color != 0 ? Textures::Get(color) : 0;
        if (color_map == 0) {
            header.dropped_world_decals++;
            return;
        }
        const Matrix box = Load<Matrix>(record);
        const Matrix world_view = Multiply(box, view.view);
        const Matrix world_view_projection = Multiply(world_view, view.projection);
        const Matrix view_to_decal = Multiply(Multiply(view.inverse_view, Invert(box)), BoxToDecal);
        const float axis = std::sqrt(world_view[8] * world_view[8] + world_view[9] * world_view[9] +
                                     world_view[10] * world_view[10]);
        // The fields are written in order: the packet is write-combined.
        Scene::WorldDecal& out = world_decals[header.world_decals++];
        std::memcpy(out.world_view, world_view.data(), MatrixBytes);
        std::memcpy(out.world_view_projection, world_view_projection.data(), MatrixBytes);
        std::memcpy(out.view_to_decal, view_to_decal.data(), MatrixBytes);
        std::memcpy(out.color_scale, reinterpret_cast<const void*>(record + WorldDecalScales),
                    sizeof(out.color_scale) + sizeof(out.normal_scale) +
                        sizeof(out.color_transform) + sizeof(out.normal_transform));
        out.parallax[0] = -world_view[8] / axis;
        out.parallax[1] = -world_view[9] / axis;
        out.parallax[2] = -world_view[10] / axis;
        out.parallax[3] = Load<float>(record + WorldDecalParallax) * WorldDecalParallaxScale;
        out.roughness = Load<float>(record + WorldDecalRoughness);
        out.metallic = Load<float>(record + WorldDecalMetallic);
        out.translucency = Load<float>(record + WorldDecalTranslucency);
        out.color_map = color_map;
        out.normal_map = normal != 0 ? Textures::Get(normal) : 0;
    }

    // A Branch deformer record: the renderer's kernel sways the instance's copy of the shape's
    // vertices with the record's constants.
    void AddBranchJob(VAddr record) {
        const VAddr vertex_buffer = Load<VAddr>(record + BranchOutput);
        const VAddr shape = Load<VAddr>(record + BranchShape);
        const VAddr constants = Load<VAddr>(record + BranchConstants);
        if (vertex_buffer == 0 || shape == 0 || constants == 0 ||
            header.branch_jobs == Scene::MaxBranchJobs) {
            header.dropped_branch_jobs++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        const u32 segments = Load<u32>(constants + BranchVertexCount) / PatchVertices;
        if (mesh == nullptr || !IsBranchMesh(*mesh) || segments == 0 ||
            u64{segments} * PatchVertices * mesh->stride > mesh->vertex_bytes) {
            header.dropped_branch_jobs++;
            return;
        }
        const Deformers::Copy copy = BranchCopy(vertex_buffer, shape, *mesh);
        if (copy.state == 0) {
            header.dropped_branch_jobs++;
            return;
        }
        // The fields are written in order: the packet is write-combined.
        Scene::BranchJob& job = branch_jobs[header.branch_jobs++];
        std::memcpy(job.world, reinterpret_cast<const void*>(constants), 3 * MatrixBytes);
        job.source = mesh->vertices;
        job.destination = copy.vertices;
        job.state = copy.state;
        job.segments = segments;
        job.cycle = Load<float>(constants + BranchCycle);
        job.cluster_scale = Load<float>(constants + BranchClusterScale);
        header.branch_groups =
            std::max(header.branch_groups, (segments + BranchGroupSize - 1) / BranchGroupSize);
    }

    // A branch ribbon item: a ribbon of the item's segments along each curve of three vertices.
    void AddBranch(VAddr item) {
        const VAddr hull = Load<VAddr>(item + LeafHullConstants);
        const VAddr domain = Load<VAddr>(item + LeafDomainConstants);
        const VAddr pixel = Load<VAddr>(item + BranchPixelConstants);
        const Meshes::Mesh* mesh = nullptr;
        u64 address = 0;
        u32 vertices = 0;
        u32 texture = 0;
        if (hull == 0 || domain == 0 || pixel == 0 || header.branches == Scene::MaxBranches ||
            !ReadBranchItem(item, BranchItemVertexCount, mesh, address, vertices, texture) ||
            !IsBranchMesh(*mesh)) {
            header.dropped_branches++;
            return;
        }
        const u32 segments = std::max(static_cast<u32>(std::ceil(Load<float>(hull))), 1u);
        const u32 i = header.branches++;
        // The fields are written in order: the packet is write-combined.
        Scene::Branch& branch = branches[i];
        std::memcpy(branch.world_view_projection, reinterpret_cast<const void*>(domain),
                    MatrixBytes);
        std::memcpy(branch.eye, reinterpret_cast<const void*>(domain + BranchEye),
                    sizeof(branch.eye) + sizeof(branch.uv_range) + sizeof(branch.tip_color) +
                        sizeof(branch.root_color));
        branch.vertices = address;
        branch.stride = mesh->stride;
        branch.segments = segments;
        branch.texture = texture;
        std::memcpy(&branch.tip_width, reinterpret_cast<const void*>(domain + BranchWidths),
                    2 * sizeof(float));
        std::memcpy(&branch.alpha_reference, reinterpret_cast<const void*>(pixel),
                    BranchPixelCount * sizeof(float));
        g_branch_commands[i] = {
            .vertex_count = vertices / PatchVertices * segments * QuadVertices,
            .instance_count = 1,
            .first_vertex = 0,
            .first_instance = i,
        };
    }

    // A Branch leaf item: its instance's vertices as patches of three, with the item's constants.
    void AddLeaf(VAddr item) {
        const VAddr hull = Load<VAddr>(item + LeafHullConstants);
        const VAddr domain = Load<VAddr>(item + LeafDomainConstants);
        const VAddr pixel = Load<VAddr>(item + LeafPixelConstants);
        const Meshes::Mesh* mesh = nullptr;
        u64 address = 0;
        u32 vertices = 0;
        u32 texture = 0;
        if (hull == 0 || domain == 0 || pixel == 0 || header.leaves == Scene::MaxLeaves ||
            !ReadBranchItem(item, LeafVertexCount, mesh, address, vertices, texture)) {
            header.dropped_leaves++;
            return;
        }
        const u32 i = header.leaves++;
        // The fields are written in order: the packet is write-combined.
        Scene::Leaf& leaf = leaves[i];
        std::memcpy(leaf.world_view_projection, reinterpret_cast<const void*>(domain), MatrixBytes);
        std::memcpy(leaf.world, reinterpret_cast<const void*>(domain + LeafWorld), 2 * MatrixBytes);
        std::memcpy(leaf.tip_color, reinterpret_cast<const void*>(pixel),
                    sizeof(leaf.tip_color) + sizeof(leaf.root_color));
        leaf.vertices = address;
        leaf.stride = mesh->stride;
        leaf.position = mesh->position;
        leaf.texture = texture;
        std::memcpy(&leaf.tessellation, reinterpret_cast<const void*>(hull), 2 * sizeof(float));
        std::memcpy(&leaf.direction, reinterpret_cast<const void*>(domain + LeafShape),
                    LeafShapeValues * sizeof(float));
        std::memcpy(&leaf.alpha_reference, reinterpret_cast<const void*>(pixel + LeafPixelValues),
                    LeafPixelCount * sizeof(float));
        g_leaf_commands[header.leaf_commands++] = {
            .vertex_count = vertices,
            .instance_count = 1,
            .first_vertex = 0,
            .first_instance = i,
        };
    }

    // A leaf shadow item, drawn in each cascade its mask names as the casters are.
    void AddLeafShadow(VAddr item) {
        const u32 mask = Load<u8>(item + CasterMask) & header.cascades;
        if (mask == 0) {
            return;
        }
        const VAddr hull = Load<VAddr>(item + LeafHullConstants);
        const VAddr domain = Load<VAddr>(item + LeafDomainConstants);
        const VAddr pixel = Load<VAddr>(item + LeafPixelConstants);
        const Meshes::Mesh* mesh = nullptr;
        u64 address = 0;
        u32 vertices = 0;
        u32 texture = 0;
        if (hull == 0 || domain == 0 || pixel == 0 || header.leaves == Scene::MaxLeaves ||
            !ReadBranchItem(item, LeafVertexCount, mesh, address, vertices, texture)) {
            header.dropped_leaves++;
            return;
        }
        const u32 i = header.leaves++;
        // The fields the shadow stages read, in order: the packet is write-combined.
        Scene::Leaf& leaf = leaves[i];
        std::memcpy(leaf.world, reinterpret_cast<const void*>(domain + LeafShadowWorld),
                    MatrixBytes);
        leaf.vertices = address;
        leaf.stride = mesh->stride;
        leaf.position = mesh->position;
        leaf.texture = texture;
        std::memcpy(&leaf.tessellation, reinterpret_cast<const void*>(hull), 2 * sizeof(float));
        std::memcpy(&leaf.direction, reinterpret_cast<const void*>(domain + LeafShadowShape),
                    LeafShapeValues * sizeof(float));
        leaf.alpha_reference = Load<float>(pixel);
        leaf.punch_through = Load<float>(pixel + 4);
        for (u32 c = 0; c < Scene::Cascades; c++) {
            if ((mask >> c & 1) == 0) {
                continue;
            }
            g_leaf_shadow_commands[c * Scene::MaxLeaves + header.leaf_shadow_commands[c]++] = {
                .vertex_count = vertices,
                .instance_count = 1,
                .first_vertex = 0,
                .first_instance = i,
            };
        }
    }

    // Remembers a Grass deformer record for the item that shares its indirect arguments.
    void AddGrassRecord(VAddr record) {
        if (grass_record_count < grass_records.size()) {
            grass_records[grass_record_count++] = record;
        }
    }

    // A grass item and its deformer record: the constants of the kernel, with those the game's
    // grass pass sets after the capture, then those of the draw. The kernel reads the shape's copy
    // of the tufts.
    void AddGrass(VAddr item, const GrassView& view) {
        const VAddr arguments = Load<VAddr>(item + GrassArguments);
        const VAddr geometry = Load<VAddr>(item + GrassGeometryConstants);
        const VAddr pixel = Load<VAddr>(item + GrassPixelConstants);
        const VAddr color = Load<VAddr>(item + GrassColor);
        const VAddr normal = Load<VAddr>(item + GrassNormal);
        const auto records = std::span{grass_records}.first(grass_record_count);
        const auto record = std::ranges::find_if(records, [arguments](VAddr candidate) {
            return Load<VAddr>(candidate + GrassRecordArguments) == arguments;
        });
        if (record == records.end() || geometry == 0 || pixel == 0 || color == 0 ||
            header.grass == Scene::MaxGrass) {
            header.dropped_grass++;
            return;
        }
        const VAddr constants = Load<VAddr>(*record + GrassRecordConstants);
        const Meshes::Mesh* mesh = Meshes::Get(Load<VAddr>(*record + GrassRecordShape));
        const u32 vertices = constants != 0 ? Load<u32>(constants + GrassVertexCount) : 0;
        const u32 color_map = Textures::Get(color);
        if (mesh == nullptr || mesh->stride != GrassVertexBytes || vertices < 2 ||
            u64{vertices} * GrassVertexBytes > mesh->vertex_bytes || color_map == 0) {
            header.dropped_grass++;
            return;
        }
        const Matrix world = Load<Matrix>(constants);
        const Matrix world_view = Multiply(world, view.view);
        const Matrix world_view_projection = Multiply(world, view.view_projection);
        const float speed = Load<float>(constants + GrassExtraForce + 12);
        const bool moving = speed > GrassMovingForce;
        if (moving) {
            grass_advance = std::max(grass_advance, 2.0f * speed * view.time_step);
        }
        const u32 tufts = vertices / 2;
        // The fields are written in order: the packet is write-combined.
        Scene::Grass& out = grass[header.grass];
        std::memcpy(out.world, reinterpret_cast<const void*>(constants), 2 * MatrixBytes);
        std::memcpy(out.world_view, world_view.data(), MatrixBytes);
        std::memcpy(out.world_view_projection, world_view_projection.data(), MatrixBytes);
        std::memcpy(out.extra_force, reinterpret_cast<const void*>(constants + GrassExtraForce),
                    sizeof(out.extra_force));
        out.tufts = mesh->vertices;
        out.tuft_count = tufts;
        std::memcpy(&out.clip_min, reinterpret_cast<const void*>(constants + GrassClipRange),
                    2 * sizeof(float));
        std::memcpy(&out.coherency, reinterpret_cast<const void*>(constants + GrassShapeValues),
                    GrassShapeValueCount * sizeof(float));
        out.cluster_scale = view.cluster_scale;
        out.phase = moving ? g_grass_phase : 0.0f;
        std::memcpy(out.draw_world_view_projection, reinterpret_cast<const void*>(geometry),
                    MatrixBytes);
        std::memcpy(out.eye, reinterpret_cast<const void*>(geometry + GrassEye), sizeof(out.eye));
        std::memcpy(out.tip_color, reinterpret_cast<const void*>(pixel),
                    sizeof(out.tip_color) + sizeof(out.root_color));
        out.width = Load<float>(geometry + GrassWidth);
        std::memcpy(&out.alpha_reference, reinterpret_cast<const void*>(pixel + GrassPixelValues),
                    GrassPixelCount * sizeof(float));
        out.color_map = color_map;
        out.normal_map = normal != 0 ? Textures::Get(normal) : 0;
        header.grass++;
        header.grass_groups =
            std::max(header.grass_groups, (tufts + GrassGroupSize - 1) / GrassGroupSize);
    }

    void AdvanceGrassPhase() {
        g_grass_phase = std::fmod(g_grass_phase + grass_advance, FullTurn);
    }

    // The game draws a caster in each cascade its mask names, with the index range of the
    // category's override when there is one.
    void AddCaster(VAddr item, const CasterCategory& category) {
        const u32 mask = Load<u8>(item + CasterMask) & header.cascades;
        if (mask == 0) {
            return;
        }
        const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr world = Load<VAddr>(item + CasterWorld);
        const VAddr range = Load<VAddr>(item + category.range);
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        if (submesh == 0 || index_buffer == 0 || world == 0 ||
            header.casters == Scene::MaxCasters) {
            header.dropped_casters++;
            return;
        }
        u32 first = 0;
        u32 count = 0;
        if (range == 0) {
            if (Load<u16>(submesh + SubmeshRanges) == 0) {
                return;
            }
            first = Load<u32>(submesh + SubmeshFirstIndex) + Load<u32>(submesh + SubmeshRange);
            count = Load<u32>(submesh + SubmeshRange + 4);
        } else {
            first = Load<u32>(submesh + SubmeshFirstIndex);
            count = Load<u32>(range + SubmeshRange) - (Load<u32>(submesh + SubmeshRange) + first) +
                    Load<u32>(range + SubmeshRange + 4) + Load<u32>(range + SubmeshFirstIndex);
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr || count == 0 || u64{first} + count > mesh->indices) {
            header.dropped_casters++;
            return;
        }
        Skin skin;
        u64 vertices = mesh->vertices;
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (vertex_buffer != shape + ShapeVertexBuffer) {
            vertices = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
            if (vertices == 0) {
                header.dropped_casters++;
                return;
            }
            header.deformed_casters++;
        } else if (!ReadSkin(item, shape, *mesh, skin)) {
            header.dropped_casters++;
            return;
        }
        header.skinned_casters += skin.bones != 0;
        const u8 flags = Load<u8>(item + CasterFlags);
        const u32 table = (flags & 0x10) != 0 ? 1 : (flags & 0x20) != 0 ? 2 : 0;
        // A reference below 0 passes every texel: the caster is opaque.
        float alpha_reference = -1.0f;
        u32 texture = 0;
        if (category.constants != None && mesh->texcoord != 0) {
            const VAddr constants = Load<VAddr>(item + category.constants);
            const VAddr object = Load<VAddr>(item + category.texture);
            if (constants != 0 && object != 0) {
                alpha_reference = Load<float>(constants);
                texture = Textures::Get(object);
            }
        }
        const bool alpha_tested = alpha_reference >= 0.0f && texture != 0;
        const u32 list = (alpha_tested ? 2 : 0) + ((flags & 0x08) != 0 ? 0 : 1);
        const u32 i = header.casters++;
        Scene::Caster& caster = casters[i];
        std::memcpy(caster.world, reinterpret_cast<const void*>(world), MatrixBytes);
        caster.vertices = vertices;
        caster.stride = mesh->stride;
        caster.position = mesh->position;
        caster.table = table;
        caster.texture = texture;
        caster.texcoord = mesh->texcoord;
        caster.alpha_reference = alpha_reference;
        caster.skin = skin.stream;
        caster.palette = skin.palette;
        caster.bones = skin.bones;
        for (u32 c = 0; c < Scene::Cascades; c++) {
            if ((mask >> c & 1) == 0) {
                continue;
            }
            header.caster_indices[c] += count;
            u32& n = header.caster_commands[c][list];
            g_caster_commands[(c * Scene::CasterLists + list) * Scene::MaxCasters + n++] = {
                .index_count = count,
                .instance_count = 1,
                .first_index = mesh->first_index + first,
                .vertex_offset = 0,
                .first_instance = i,
            };
        }
    }

    // A Skin deformer record: the renderer skins the instance's copy as the game's Skin program
    // skins the instance.
    void AddSkin(VAddr record) {
        const VAddr vertex_buffer = Load<VAddr>(record + SkinOutput);
        const VAddr shape = Load<VAddr>(record + SkinShape);
        const VAddr constants = Load<VAddr>(record + SkinConstants);
        const VAddr palette = Load<VAddr>(record + SkinPalette);
        const u32 bones = Load<u32>(record + SkinBones);
        if (vertex_buffer == 0 || shape == 0 || constants == 0 || palette == 0 || bones == 0 ||
            Load<VAddr>(record + SkinSource) != shape + ShapeVertexBuffer ||
            header.skin_jobs == Scene::MaxSkinJobs) {
            header.dropped_skins++;
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        const auto [vertices, stride, normal, tangent] = Load<std::array<u32, 4>>(constants);
        if (mesh == nullptr || mesh->skin == 0 || mesh->position != SkinPosition ||
            Load<VAddr>(record + SkinStream) != Load<VAddr>(shape + ShapeSkin) + SkinVertexBuffer ||
            stride != mesh->stride || u64{vertices} * stride > mesh->vertex_bytes ||
            normal % 4 != 0 || tangent % 4 != 0 || normal + 4 > stride || tangent + 8 > stride) {
            header.dropped_skins++;
            return;
        }
        const u64 output = Deformers::Get(vertex_buffer, shape, *mesh, frame).vertices;
        u64 palette_address = 0;
        if (output == 0 || !CopyPalette(palette, bones, palette_address)) {
            header.dropped_skins++;
            return;
        }
        skin_jobs[header.skin_jobs++] = {
            .source = mesh->vertices,
            .destination = output,
            .skin = mesh->skin,
            .palette = palette_address,
            .bones = bones,
            .vertices = vertices,
            .stride = stride,
            .normal = normal,
            .tangent = tangent,
        };
        header.skin_groups =
            std::max(header.skin_groups, (vertices + SkinGroupSize - 1) / SkinGroupSize);
    }

    // A SoftBody deformer record: the renderer simulates the instance's cloth with its own state
    // and writes it into the instance's copy, after any skin job of the instance.
    void AddSoftBody(VAddr record, const GridConstants& grid) {
        const VAddr vertex_buffer = Load<VAddr>(record + SoftBodyOutput);
        const VAddr shape = Load<VAddr>(record + SoftBodyShape);
        const VAddr simulate = Load<VAddr>(record + SoftBodySimulate);
        const VAddr constants = simulate != 0 ? simulate : Load<VAddr>(record + SoftBodyReset);
        const VAddr write = Load<VAddr>(record + SoftBodyWrite);
        if (vertex_buffer == 0 || shape == 0 || constants == 0 || write == 0) {
            DropSoftBody(Scene::SoftBodyDrop::Record);
            return;
        }
        if (header.soft_body_jobs == Scene::MaxSoftBodyJobs) {
            DropSoftBody(Scene::SoftBodyDrop::JobCap);
            return;
        }
        const Meshes::Mesh* mesh = Meshes::Get(shape);
        if (mesh == nullptr) {
            DropSoftBody(Scene::SoftBodyDrop::Mesh);
            return;
        }
        const VAddr object = Load<VAddr>(shape + ShapeSoftBody);
        const bool skinned = Load<VAddr>(record + SoftBodyPalette) != 0;
        if (object == 0 || mesh->particles == 0 || mesh->position != SkinPosition ||
            mesh->stride < MinSoftBodyStride ||
            Load<VAddr>(record + SoftBodyParticles) != object + ObjectParticles ||
            Load<VAddr>(record + SoftBodyConstraints) != object + ObjectConstraints ||
            Load<VAddr>(record + SoftBodyMap) != object + ObjectMap ||
            (!skinned && Load<VAddr>(record + SoftBodySource) != shape + ShapeVertexBuffer)) {
            DropSoftBody(Scene::SoftBodyDrop::Record);
            return;
        }
        Scene::SoftBodyJob job{.reset = simulate == 0};
        std::memcpy(job.constants, reinterpret_cast<const void*>(constants),
                    (job.reset ? ResetWords : SimulateWords) * 4);
        std::memcpy(job.write, reinterpret_cast<const void*>(write), WriteWords * 4);
        const u32 particles = job.constants[job.reset ? ResetParticles : SimulateParticles];
        const u32 stride = job.constants[job.reset ? ResetStride : SimulateStride];
        const u32 vertices = job.write[WriteVertices];
        if (particles != mesh->particle_count || stride != mesh->stride ||
            job.write[WriteStride] != stride || u64{vertices} * stride > mesh->vertex_bytes ||
            (!job.reset && job.constants[SimulateIterations] > MaxIterations)) {
            DropSoftBody(Scene::SoftBodyDrop::Constants);
            return;
        }
        const Deformers::Copy copy = Deformers::Get(vertex_buffer, shape, *mesh, frame);
        if (copy.state == 0) {
            DropSoftBody(Scene::SoftBodyDrop::Copy);
            return;
        }
        if (!job.reset) {
            std::memcpy(&job.constants[SimulateGrid], grid.matrix.data(), sizeof(grid.matrix));
            job.constants[SimulateSpheres] = grid.spheres;
            std::memcpy(&job.constants[SimulateFade], grid.fade.data(), sizeof(grid.fade));
        }
        job.skinned = skinned ? copy.vertices : mesh->vertices;
        job.particles = mesh->particles;
        job.constraints = mesh->constraints;
        job.map = mesh->particle_map;
        job.state = copy.state;
        job.destination = copy.vertices;
        std::memcpy(&soft_body_jobs[header.soft_body_jobs++], &job, sizeof(job));
        header.soft_body_groups = std::max(header.soft_body_groups,
                                           (particles + SoftBodyGroupSize - 1) / SoftBodyGroupSize);
        header.write_groups =
            std::max(header.write_groups, (vertices + SoftBodyGroupSize - 1) / SoftBodyGroupSize);
    }

private:
    struct Skin {
        u64 stream = 0;
        u64 palette = 0;
        u32 bones = 0;
    };

    void DropSoftBody(Scene::SoftBodyDrop reason) {
        header.dropped_soft_bodies[static_cast<u32>(reason)]++;
    }

    // Writes a CH_Main item's decals to the packet and returns their index plus 1; 0 when the
    // material has none or its texture is not converted.
    u32 AddDecal(VAddr constants, VAddr pixel_constants, VAddr object) {
        if (object == 0) {
            return 0;
        }
        const auto scales = Load<std::array<float, 6>>(constants + DecalScales);
        if (std::ranges::all_of(scales, [](float scale) { return scale == 0.0f; })) {
            return 0;
        }
        const u32 texture = Textures::Get(object);
        if (texture == 0) {
            return 0;
        }
        if (header.decals == Scene::MaxDecals) {
            header.dropped_decals++;
            return 0;
        }
        // The fields are written in order: the packet is write-combined.
        Scene::Decal& decal = decals[header.decals];
        std::memcpy(&decal, reinterpret_cast<const void*>(constants + DecalPositions),
                    DecalConstantBytes);
        decal.texture = texture;
        decal.color_alpha = Load<u32>(pixel_constants + AlphaStepFlag) == 0 ? 1 : 0;
        std::memcpy(decal.gradation,
                    reinterpret_cast<const void*>(pixel_constants + GradationColor),
                    sizeof(decal.gradation));
        return ++header.decals;
    }

    // Writes a G2_BG_Main item's material to the packet and returns its index plus 1; 0 when the
    // packet is full.
    u32 AddMainMaterial(VAddr item, u32 flags, VAddr vertex_constants, VAddr pixel_constants,
                        const Meshes::Mesh& mesh) {
        if (header.materials == Scene::MaxMaterials) {
            header.dropped_materials++;
            return 0;
        }
        const auto handle = [item](u64 field) {
            const VAddr object = Load<VAddr>(item + field);
            return object != 0 ? Textures::Get(object) : 0;
        };
        const bool layered = mesh.vertex_color != 0;
        // The fields are written in order: the packet is write-combined.
        Scene::Material& out = materials[header.materials];
        std::memcpy(&out, reinterpret_cast<const void*>(pixel_constants), MainPixelConstantBytes);
        std::memcpy(out.gradation_low,
                    reinterpret_cast<const void*>(vertex_constants + MainGradation),
                    sizeof(out.gradation_low) + sizeof(out.gradation_high));
        out.normal_map = mesh.tangent != 0 ? handle(MainNormalMap) : 0;
        out.layered_color = layered ? handle(MainLayeredColor) : 0;
        out.layered_normal = layered && mesh.tangent != 0 ? handle(MainLayeredNormal) : 0;
        out.mask = handle(MainMask);
        out.hatching = (flags & MainFlagNoHatching) == 0 ? hatching : 0;
        out.flags = (layered ? Scene::MaterialLayered : 0) |
                    ((flags & MainFlagGradation) != 0 ? Scene::MaterialGradation : 0);
        return ++header.materials;
    }

    // Writes a G2_BG_Layer item's layers to the packet, and a material that names them and holds
    // its gradation and hatching. Returns the material's index plus 1; 0 when the packet is full.
    u32 AddLayers(VAddr item, VAddr vertex_constants, VAddr pixel_constants) {
        if (header.materials == Scene::MaxMaterials ||
            header.layer_materials == Scene::MaxLayerMaterials) {
            header.dropped_materials++;
            return 0;
        }
        const auto handles = [item](u64 first, u32(&out)[4]) {
            for (u32 layer = 0; layer < 4; layer++) {
                const VAddr object = Load<VAddr>(item + first + layer * sizeof(VAddr));
                out[layer] = object != 0 ? Textures::Get(object) : 0;
            }
        };
        // The fields are written in order: the packet is write-combined.
        Scene::LayerMaterial& layers = layer_materials[header.layer_materials];
        std::memcpy(layers.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    sizeof(layers.pixel_constants));
        handles(LayerColorMaps, layers.color_maps);
        handles(LayerNormalMaps, layers.normal_maps);
        Scene::Material& out = materials[header.materials];
        std::memcpy(out.gradation_low,
                    reinterpret_cast<const void*>(vertex_constants + MainGradation),
                    sizeof(out.gradation_low) + sizeof(out.gradation_high));
        out.hatching = (Load<u32>(item + MainFlags) & MainFlagNoHatching) == 0 ? hatching : 0;
        out.flags = Scene::MaterialLayers;
        out.layer_material = header.layer_materials++;
        return ++header.materials;
    }

    // Writes a G2_BG_Gem item's constants and maps to the packet and returns the index of its
    // Material plus 1.
    u32 AddGem(VAddr item, VAddr pixel_constants) {
        if (header.materials == Scene::MaxMaterials ||
            header.layer_materials == Scene::MaxLayerMaterials) {
            header.dropped_materials++;
            return 0;
        }
        const auto handle = [item](u64 field) {
            const VAddr object = Load<VAddr>(item + field);
            return object != 0 ? Textures::Get(object) : 0;
        };
        // The fields are written in order: the packet is write-combined.
        Scene::LayerMaterial& gem = layer_materials[header.layer_materials];
        std::memcpy(gem.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    GemPixelBytes);
        gem.color_maps[0] = handle(GemColorMap);
        gem.normal_maps[0] = handle(GemNormalMap);
        Scene::Material& out = materials[header.materials];
        out.flags = Scene::MaterialGem;
        out.layer_material = header.layer_materials++;
        return ++header.materials;
    }

    // Writes a G2_CH_Main item's constants to the packet and returns their index plus 1.
    u32 AddCharacter(VAddr item, VAddr vertex_constants, VAddr pixel_constants) {
        const auto handle = [item](u64 field) {
            const VAddr object = Load<VAddr>(item + field);
            return object != 0 ? Textures::Get(object) : 0;
        };
        // The fields are written in order: the packet is write-combined.
        Scene::Character& out = characters[header.characters];
        std::memcpy(out.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    sizeof(out.pixel_constants));
        std::memcpy(out.bend_position,
                    reinterpret_cast<const void*>(vertex_constants + CharacterBendPosition),
                    sizeof(out.bend_position));
        out.bend_range = Load<float>(vertex_constants + CharacterBend);
        out.bend_bias = Load<float>(vertex_constants + CharacterBend + 4);
        out.normal_map = handle(CharacterNormalMap);
        out.reflection = handle(CharacterReflection);
        out.detail = handle(CharacterDetail);
        return ++header.characters;
    }

    // Writes a G2_CH_Interior item's constants and cube maps to a Character and returns its index
    // plus 1. A cube map not converted yet has handle 0.
    u32 AddInterior(VAddr item, VAddr pixel_constants) {
        const auto cube = [item](u64 field) {
            const VAddr object = Load<VAddr>(item + field);
            return object != 0 ? Textures::GetCube(object) : 0;
        };
        // The fields are written in order: the packet is write-combined.
        Scene::Character& out = characters[header.characters];
        std::memcpy(out.pixel_constants, reinterpret_cast<const void*>(pixel_constants),
                    InteriorPixelBytes);
        out.bend_bias = 0.0f;
        out.normal_map = cube(InteriorCube0);
        out.reflection = cube(InteriorCube1);
        return ++header.characters;
    }

    // Copies a palette into the packet once for each run of users that share it.
    bool CopyPalette(VAddr palette, u32 bones, u64& address) {
        if (palette != last_palette || bones != last_bones) {
            const u32 rows = bones * 3;
            if (header.palette_rows + rows > Scene::MaxPaletteRows) {
                return false;
            }
            std::memcpy(palettes + u64{header.palette_rows} * 16,
                        reinterpret_cast<const void*>(palette), bones * BoneBytes);
            last_palette = palette;
            last_bones = bones;
            last_address = palettes_address + u64{header.palette_rows} * 16;
            header.palette_rows += rows;
        }
        address = last_address;
        return true;
    }

    // The skin of an item whose shape is skinned, with the item's palette in the packet. False
    // when the item cannot be drawn.
    bool ReadSkin(VAddr item, VAddr shape, const Meshes::Mesh& mesh, Skin& skin) {
        const VAddr stream = Load<VAddr>(item + ItemSkin);
        if (mesh.skin == 0 && stream == 0) {
            skin = {};
            return true;
        }
        const u32 bones = Load<u16>(item + ItemBones);
        const VAddr palette = Load<VAddr>(item + ItemPalette);
        if (mesh.skin == 0 || stream != Load<VAddr>(shape + ShapeSkin) + SkinVertexBuffer ||
            bones == 0 || palette == 0) {
            return false;
        }
        u64 address = 0;
        if (!CopyPalette(palette, bones, address)) {
            return false;
        }
        skin = {.stream = mesh.skin, .palette = address, .bones = bones};
        return true;
    }

    // A Branch shape's positions as the kernel and the ribbons read them.
    static bool IsBranchMesh(const Meshes::Mesh& mesh) {
        return mesh.stride == BranchVertexBytes && (mesh.position & 0xff) == 0 &&
               (mesh.position >> 8) != PositionHalfFloat;
    }

    // The renderer's copy of a Branch instance, with a timer, a phase and a word unused per curve.
    Deformers::Copy BranchCopy(VAddr vertex_buffer, VAddr shape, const Meshes::Mesh& mesh) {
        return Deformers::Get(vertex_buffer, shape, mesh, frame,
                              mesh.vertex_bytes / mesh.stride / PatchVertices * BranchStateBytes);
    }

    // The mesh, vertices' address, whole patches of vertices and texture of a Branch item. False
    // when the item cannot be drawn: an untextured one would fill its quads with white.
    bool ReadBranchItem(VAddr item, u64 count_field, const Meshes::Mesh*& mesh, u64& address,
                        u32& vertices, u32& texture) {
        const VAddr index_buffer = Load<VAddr>(item + ItemIndexBuffer);
        const VAddr object = Load<VAddr>(item + LeafTexture);
        vertices = Load<u32>(item + count_field) / PatchVertices * PatchVertices;
        if (index_buffer == 0 || object == 0 || vertices == 0) {
            return false;
        }
        const VAddr shape = index_buffer - ShapeIndexBuffer;
        mesh = Meshes::Get(shape);
        if (mesh == nullptr || mesh->stride == 0 || vertices > mesh->vertex_bytes / mesh->stride) {
            return false;
        }
        texture = Textures::Get(object);
        if (texture == 0) {
            return false;
        }
        address = mesh->vertices;
        const VAddr vertex_buffer = Load<VAddr>(item + ItemVertexBuffer);
        if (vertex_buffer != 0 && vertex_buffer != shape + ShapeVertexBuffer) {
            const u64 copy = BranchCopy(vertex_buffer, shape, *mesh).vertices;
            address = copy != 0 ? copy : address;
        }
        return true;
    }

    Scene::Header header{};
    u64 frame;
    std::chrono::steady_clock::time_point part_start;
    u8* packet;
    Scene::Draw* draws;
    Scene::Caster* casters;
    u8* palettes;
    u64 palettes_address;
    Scene::SkinJob* skin_jobs;
    Scene::SoftBodyJob* soft_body_jobs;
    Scene::Decal* decals;
    Scene::Material* materials;
    Scene::Character* characters;
    Scene::Contour* contours;
    Scene::Leaf* leaves;
    Scene::LayerMaterial* layer_materials;
    Scene::Grass* grass;
    Scene::BranchJob* branch_jobs;
    Scene::Branch* branches;
    Scene::WorldDecal* world_decals;
    Scene::GlassMaterial* glass_materials;
    std::array<VAddr, Scene::MaxGrass> grass_records;
    u32 grass_record_count = 0;
    float grass_advance = 0.0f;
    u32 hatching = 0;
    VAddr last_palette = 0;
    u32 last_bones = 0;
    u64 last_address = 0;
};

// Items are scattered through the pool and point at scattered objects. The loops prefetch the
// item Lookahead places ahead, and the objects of the item half as far ahead.
constexpr s32 Lookahead = 8;
constexpr u64 LineBytes = 64;

void Prefetch(VAddr address) {
    __builtin_prefetch(reinterpret_cast<const void*>(address));
}

// The submesh, the shape's handle (just before its index buffer object) and the matrices of an
// item, and of an item with pixel constants, the second matrix and those constants.
void PrefetchObjects(VAddr item, u64 matrices, u64 pixel_constants) {
    const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
    Prefetch(submesh);
    Prefetch(submesh + SubmeshRange);
    Prefetch(Load<VAddr>(item + ItemIndexBuffer) - 4);
    const VAddr constants = Load<VAddr>(item + matrices);
    Prefetch(constants);
    if (pixel_constants == None) {
        return;
    }
    Prefetch(constants + LineBytes);
    const VAddr pixels = Load<VAddr>(item + pixel_constants);
    Prefetch(pixels);
    Prefetch(pixels + LineBytes);
    Prefetch(pixels + 2 * LineBytes);
}

template <typename ItemAt, typename Visit>
void ForEachItem(s32 count, u64 matrices, u64 pixel_constants, ItemAt item_at, Visit visit) {
    for (s32 i = 0; i < count; i++) {
        if (i + Lookahead < count) {
            Prefetch(item_at(i + Lookahead));
            // An item with pixel constants holds its pointers in its second line.
            if (pixel_constants != None) {
                Prefetch(item_at(i + Lookahead) + LineBytes);
            }
        }
        if (i + Lookahead / 2 < count) {
            PrefetchObjects(item_at(i + Lookahead / 2), matrices, pixel_constants);
        }
        visit(item_at(i));
    }
}

// Writes the cascades into the packet's shadow block and the header's cascade fields.
void WriteShadows(VAddr view, u8* packet, Writer& writer) {
    auto* shadows = reinterpret_cast<Scene::Shadows*>(packet + Scene::ShadowsOffset);
    std::memcpy(shadows->inverse_view, reinterpret_cast<const void*>(view + ViewInverse),
                MatrixBytes);
    u32 mask = 0;
    for (u32 c = 0; c < Scene::Cascades; c++) {
        const VAddr cascade = view + ViewCascades + c * CascadeBytes;
        mask |= (Load<u32>(cascade) != 0 ? 1u : 0u) << c;
        writer.SetDepthBias(c, Load<float>(cascade + CascadeBiasConstant),
                            Load<float>(cascade + CascadeBiasSlope));
        for (u32 table = 0; table < Scene::CasterTables; table++) {
            VAddr matrix = Load<VAddr>(view + ViewCasterTables + table * CasterTableBytes + c * 8);
            if (matrix == 0) {
                matrix = cascade + CascadeViewProjection;
            }
            std::memcpy(shadows->view_projection[c * Scene::CasterTables + table],
                        reinterpret_cast<const void*>(matrix), MatrixBytes);
        }
    }
    writer.SetCascades(mask);
    // Clip depth grows away from the light, so the first cascade's depth row, negated, points
    // toward it. Row-vector matrices: a direction d maps to d * M.
    const auto vp = Load<std::array<float, 16>>(view + ViewCascades + CascadeViewProjection);
    const auto v = Load<std::array<float, 16>>(view + ViewMatrix);
    const std::array<float, 3> toward{-vp[2], -vp[6], -vp[10]};
    std::array<float, 4> direction{};
    for (u32 j = 0; j < 3; j++) {
        direction[j] = toward[0] * v[j] + toward[1] * v[4 + j] + toward[2] * v[8 + j];
    }
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                   direction[2] * direction[2]);
    if (length > 0.0f) {
        for (u32 j = 0; j < 3; j++) {
            direction[j] /= length;
        }
    }
    std::memcpy(shadows->light_direction, direction.data(), sizeof(direction));
    std::memcpy(shadows->key_light_color, reinterpret_cast<const void*>(view + ViewKeyLight),
                sizeof(shadows->key_light_color) + sizeof(shadows->key_light_direction));
}

// The two heaviest records of a list that accept selects, as the frame graph picks them.
template <typename Accept>
std::pair<VAddr, VAddr> HeaviestTwo(VAddr list, s32 count, u64 bytes, u64 weight_field,
                                    Accept accept) {
    VAddr first = 0;
    VAddr second = 0;
    for (s32 i = 0; i < count; i++) {
        const VAddr record = list + i * bytes;
        if (!accept(record)) {
            continue;
        }
        const float weight = Load<float>(record + weight_field);
        if (first == 0) {
            first = record;
        } else if (weight > Load<float>(first + weight_field)) {
            second = first;
            first = record;
        } else if (second == 0 || weight > Load<float>(second + weight_field)) {
            second = record;
        }
    }
    return {first, second};
}

// The sky of the game's sky pass: the two heaviest skies of the view's list with a cube map,
// blended by weight. With no sky the pass fills the scene with the background colour.
void WriteSky(VAddr frame_graph, VAddr view, u8* packet, Writer& writer) {
    const auto background = Load<std::array<float, 3>>(view + ViewBackground);
    const auto [first, second] =
        HeaviestTwo(Load<VAddr>(view + ViewSkies), Load<s32>(view + ViewSkyCount), SkyBytes,
                    SkyWeight, [](VAddr sky) { return Load<VAddr>(sky + SkyCube) != 0; });
    const u32 cube = first != 0 ? Textures::GetCube(Load<VAddr>(first + SkyCube)) : 0;
    writer.SetSky(cube, background);
    if (cube == 0) {
        return;
    }
    Scene::Sky sky{.cubes = {cube, 0}};
    const auto rotation = Load<Matrix>(view + ViewSkyRotation);
    auto camera = Load<Matrix>(view + ViewInverse);
    camera[12] = camera[13] = camera[14] = 0.0f;
    const Matrix inverse =
        Multiply(Load<Matrix>(view + ViewInverseProjection), Multiply(camera, rotation));
    std::memcpy(sky.inverse_view_projection, inverse.data(), sizeof(inverse));
    std::memcpy(sky.transforms[0], reinterpret_cast<const void*>(first), MatrixBytes);
    const float first_weight = Load<float>(first + SkyWeight);
    const u32 second_cube = second != 0 ? Textures::GetCube(Load<VAddr>(second + SkyCube)) : 0;
    float opacity = Load<float>(view + ViewCloudOpacity);
    if (second != 0) {
        // The clouds of two skies do not match: they thin out halfway through the blend.
        const float second_weight = Load<float>(second + SkyWeight);
        const float share = second_weight / (second_weight + first_weight);
        opacity *= std::abs(share - 0.5f) * 2.0f;
        if (second_cube != 0) {
            std::memcpy(sky.transforms[1], reinterpret_cast<const void*>(second), MatrixBytes);
            sky.cubes[1] = second_cube;
            sky.blend = share;
        }
    }
    const auto sun_color = Load<std::array<float, 3>>(view + ViewSunColor);
    std::memcpy(sky.sun_color, sun_color.data(), sizeof(sun_color));
    const auto travel = Load<std::array<float, 3>>(view + ViewSunTravel);
    for (u32 j = 0; j < 3; j++) {
        sky.sun_direction[j] =
            -(travel[0] * rotation[j] + travel[1] * rotation[4 + j] + travel[2] * rotation[8 + j]);
    }
    const float sun_size = Load<float>(view + ViewSunSize);
    sky.sun_size = sun_size == 0.0f ? -10.0f : sun_size * DegreesToRadians * 0.5f;
    sky.sun_outer = 1.0f / std::max(Load<float>(view + ViewSunOuter) * DegreesToRadians, 1e-6f);
    const VAddr cloud = Load<VAddr>(first + SkyCloud);
    if (cloud != 0) {
        // The game's default cloud texture is white, as handle 0 is.
        sky.cloud = Textures::Get(cloud);
        opacity = sky.cloud != 0 ? opacity : 0.0f;
    }
    sky.cloud_color[0] = sky.cloud_color[1] = sky.cloud_color[2] = 1.0f;
    sky.cloud_color[3] = opacity;
    const auto scroll = Load<std::array<float, 2>>(frame_graph + FrameGraphCloudScroll);
    const float cloud_scale = Load<float>(view + ViewCloudScale);
    const float cloud_size = Load<float>(view + ViewCloudSize);
    const float texcoord_scale = Load<float>(view + ViewCloudTexcoord) / cloud_size;
    const std::array<float, 4> layer0{cloud_scale, scroll[0], scroll[1], texcoord_scale};
    const std::array<float, 4> layer1{cloud_scale + Load<float>(view + ViewCloudScaleStep),
                                      cloud_size * 0.5f + scroll[0], scroll[1], texcoord_scale};
    std::memcpy(sky.cloud_layers[0], layer0.data(), sizeof(layer0));
    std::memcpy(sky.cloud_layers[1], layer1.data(), sizeof(layer1));
    sky.cloud_fade[0] = cloud_size * 0.5f;
    sky.cloud_fade[1] = 1.0f / cloud_size;
    std::memcpy(packet + Scene::SkyOffset, &sky, sizeof(sky));
}

// The values the game's exposure, Tonemap and Composite passes set up on the draw thread after the
// capture, all from the view.
void WriteGrade(VAddr view, u8* packet) {
    Scene::Grade grade{};
    const auto exposure = Load<std::array<float, 4>>(view + ViewExposure);
    grade.exposure = exposure[0];
    grade.adaption_range = exposure[1];
    grade.adaption_speed =
        exposure[2] > 1.0f ? 1.0f : 1.0f - std::exp(-exposure[2] * AdaptionFrame);
    grade.key_value = exposure[3];
    const auto curves = Load<std::array<float, 4 * CurveKnots>>(view + ViewCurves);
    for (u32 channel = 0; channel < 3; channel++) {
        for (u32 knot = 0; knot < CurveKnots; knot++) {
            // Every curve is a difference from the straight line through the knots.
            const float value = curves[channel * CurveKnots + knot] +
                                curves[3 * CurveKnots + knot] - static_cast<float>(knot) / 6.0f;
            grade.knots[channel][knot] = std::clamp(value, 0.0f, 1.0f);
        }
    }
    const float lens = Load<float>(view + ViewLens);
    const float fluctuation = Load<float>(view + ViewFluctuation);
    if (lens <= 0.0f) {
        grade.distortion[0] = 1.0f;
        grade.distortion[1] = 1.0f;
        grade.distortion[2] = lens * LensInwardBend;
    } else {
        grade.distortion[0] = lens * LensOutwardScale;
        grade.distortion[1] = LensOutwardShape;
        grade.distortion[2] = 1.0f;
    }
    grade.fluctuation[0] = static_cast<float>(std::fmod(Load<double>(view + ViewTime) * 1.5, 6.28));
    grade.fluctuation[1] = std::max(0.0f, fluctuation * 2.0f);
    grade.saturation = Load<float>(view + ViewSaturation);
    grade.distorted = lens > 0.0f || fluctuation > 0.0f ? 1 : 0;
    const auto low = Load<std::array<float, 4>>(view + ViewBloomLow);
    const auto high = Load<std::array<float, 4>>(view + ViewBloomHigh);
    for (u32 i = 0; i < 4; i++) {
        grade.bloom_low[i] = low[i];
        grade.bloom_high[i] = high[i] == low[i] ? high[i] + BloomLevelStep : high[i];
    }
    grade.bloom_correction[0] = 1.0f / Load<float>(view + ViewBloomPower);
    std::memcpy(&grade.bloom_correction[1],
                reinterpret_cast<const void*>(view + ViewBloomCorrection), 3 * sizeof(float));
    std::memcpy(grade.bloom_depths, reinterpret_cast<const void*>(view + ViewBloomDepths),
                sizeof(grade.bloom_depths));
    const float strength = Load<float>(view + ViewBloomStrength);
    const auto color = Load<std::array<float, 3>>(view + ViewBloomColor);
    for (u32 i = 0; i < 3; i++) {
        grade.bloom_strength[i] = strength * color[i];
    }
    grade.bloom_strength[3] = strength != 0.0f ? 1.0f : 0.0f;
    grade.bloom_width[0] = Load<float>(view + ViewBloomWidth);
    grade.bloom_width[1] = Load<float>(view + ViewBloomWidth + 4);
    if (grade.bloom_width[1] == grade.bloom_width[0]) {
        grade.bloom_width[1] += BloomWidthStep;
    }
    grade.bloom_feedback = Load<float>(view + ViewBloomFeedback);
    std::memcpy(packet + Scene::GradeOffset, &grade, sizeof(grade));
}

// The fog's state on the draw thread: the jitter clocks, and the inputs of the maps and their
// darkness with the counts of their changes.
struct FogState {
    float time = 0.0f;
    float shadow_time = 0.0f;
    std::array<std::array<float, 7>, 2> maps{};
    std::array<float, 3> sun{};
    std::array<u32, 2> map_changes{};
    std::array<u32, 2> darkness_changes{};
};
FogState g_fog;

// The fog's values for the header: its flags, the counts of map changes and the noise volumes.
struct FogHeader {
    u32 flags;
    std::array<u32, 2> maps;
    std::array<u32, 2> darkness;
    std::array<u64, 2> noise;
};

// The values the game's FogCloud, FogDist, FogCast and FogRender passes set up on the draw thread,
// from the view and the frame graph.
FogHeader WriteFog(VAddr frame_graph, VAddr view, u8* packet) {
    Scene::Fog fog{};
    const auto number = [view](u64 field) { return Load<float>(view + field); };
    // The game's density from an opacity: -ln(1 - v) / 40, or 1 for an opacity of 1.
    const auto density = [](float v) { return v < 1.0f ? -0.025f * std::log(1.0f - v) : 1.0f; };
    const Matrix inverse_view = Load<Matrix>(view + ViewInverse);
    const Matrix to_fog = Multiply(inverse_view, Load<Matrix>(view + ViewFogSpace));
    const Matrix to_rotated = Multiply(inverse_view, Load<Matrix>(view + ViewSkyRotation));
    std::memcpy(fog.view_to_fog, to_fog.data(), sizeof(to_fog));
    std::memcpy(fog.view_to_world, inverse_view.data(), sizeof(inverse_view));
    std::memcpy(fog.view_to_rotated, to_rotated.data(), sizeof(to_rotated));
    const float depth_start = number(ViewGridNear);
    const float fog_distance = number(ViewFogDistance);
    fog.projection[0] = 1.0f / number(ViewProjectionX);
    fog.projection[1] = 1.0f / number(ViewProjectionY);
    fog.projection[2] = depth_start - fog_distance;
    fog.projection[3] = -depth_start;
    const auto cumulus_offsets = Load<std::array<float, 6>>(frame_graph + FrameGraphCumulusOffsets);
    for (u32 i = 0; i < 3; i++) {
        const float height = number(ViewCumulusHeight + i * 4);
        fog.cumulus[i][0] = number(ViewCumulusBase + i * 4) - 0.5f * height;
        fog.cumulus[i][1] = 2.0f / height;
        fog.cumulus[i][2] = density(number(ViewCumulusDensity + i * 4));
        fog.cumulus[i][3] = 1.0f / number(ViewCumulusScale + i * 4);
        fog.cumulus_offsets[i][0] = cumulus_offsets[i];
        fog.cumulus_offsets[i][1] = cumulus_offsets[3 + i];
    }
    const auto layer_offsets = Load<std::array<float, 4>>(frame_graph + FrameGraphLayerOffsets);
    const std::array<u64, 2> layer_fields{ViewStratus, ViewRegion};
    const std::array<u64, 2> layer_offset_fields{ViewStratusOffset, ViewRegionOffset};
    for (u32 i = 0; i < 2; i++) {
        const u64 field = layer_fields[i];
        const float height = number(field + 4);
        fog.layers[i][0] = number(field);
        fog.layers[i][1] = 2.0f / height;
        fog.layers[i][2] = density(number(field + 8));
        fog.layers[i][3] = 0.25f / height;
        fog.layer_offsets[i][0] = number(layer_offset_fields[i]) + layer_offsets[i * 2];
        fog.layer_offsets[i][1] = number(layer_offset_fields[i] + 4) + layer_offsets[i * 2 + 1];
        fog.layer_offsets[i][2] = number(field + 8) * number(field + 12);
    }
    const float light = 0.01f * number(ViewFogLight);
    const float least_density = number(ViewFogExtinctionDensity);
    for (u32 i = 0; i < 3; i++) {
        fog.light_color[i] = number(ViewFogLightColor + i * 4) * light;
    }
    fog.light_color[3] =
        0.1f * number(ViewFogExtinction) +
        (least_density < 1.0f ? std::log(1.0f - least_density) / (depth_start - fog_distance)
                              : 1.0f);
    for (u32 i = 0; i < 3; i++) {
        fog.light_direction[i] = number(ViewSunView + i * 4);
    }
    std::memcpy(fog.noise, reinterpret_cast<const void*>(frame_graph + FrameGraphFogScroll),
                3 * sizeof(float));
    fog.noise[3] = 0.2f * number(ViewFogNoise);
    const auto phase = [](float g, float* out) {
        out[0] = (1.0f - g * g) * 0.07957747f;
        out[1] = g * g + 1.0f;
        out[2] = g + g;
    };
    phase(number(ViewFogPhaseAir), fog.phase_air);
    phase(number(ViewFogPhaseCloud), fog.phase_cloud);
    const float cloud = std::exp(number(ViewFogCloud));
    for (u32 i = 0; i < 3; i++) {
        fog.cloud_color[i] = number(ViewFogColor + i * 4) * cloud;
    }
    fog.cloud_color[3] = 1.0f;
    fog.depth_scale = (fog_distance - depth_start) * 0.025f;
    fog.darkness_scale = number(ViewFogDarkness) * 16.0f;
    fog.noise_scale = number(ViewFogNoise + 4);
    fog.noise_attenuation = number(ViewFogNoise + 8);
    const float shadow = number(ViewFogShadow);
    const float cloud_shadow = number(ViewFogCloudShadow);
    fog.shadow_darkness = shadow > 0.0f ? 1.0f : 0.0f;
    fog.cloud_shadow = cloud_shadow;
    // The game moves its clocks inside the frame graph, after the capture.
    const float step = number(ViewTimeStep);
    const auto advance = [step](float& clock) {
        clock += step;
        clock = clock > FogClockWrap ? 0.0f : clock;
        return clock;
    };
    fog.time = advance(g_fog.time);
    const bool shadowed = shadow > 0.0f || cloud_shadow != 0.0f;
    fog.shadow_time = shadowed ? advance(g_fog.shadow_time) : g_fog.shadow_time;
    const float depth_range = number(ViewDepthRange);
    const float scale = 1.0f / (depth_start * (depth_range + depth_range));
    fog.apply_depth[0] = (depth_start - depth_range) * scale;
    fog.apply_depth[1] = (depth_range + depth_start) * scale;
    fog.apply_depth[2] = depth_start;
    fog.apply_depth[3] = 1.0f / (fog_distance - depth_start);
    std::memcpy(fog.apply_color, reinterpret_cast<const void*>(view + ViewFogColor),
                sizeof(fog.apply_color));
    std::memcpy(fog.eye, reinterpret_cast<const void*>(view + ViewRotatedEye), 3 * sizeof(float));
    fog.eye[3] = 1.0f;
    fog.puddle_height = number(ViewPuddle);
    fog.puddle_density = 0.1f * number(ViewPuddle + 4);
    fog.puddle_range = fog_distance - depth_start;
    const VAddr textures = Load<VAddr>(frame_graph + FrameGraphFogTextures);
    const auto cumulus = [textures](u32 i) {
        const VAddr object = textures != 0 && i < FogCumulusCount
                                 ? Load<VAddr>(textures + FogCumulusTextures + i * 8)
                                 : 0;
        return object != 0 ? Textures::Get(object) : 0;
    };
    fog.cumulus_texture = cumulus(0);
    const std::array<u64, 2> bake_fields{ViewStratusBake, ViewRegionBake};
    const auto sun = Load<std::array<float, 3>>(view + ViewSunTravel);
    const bool sun_moved = sun != g_fog.sun;
    g_fog.sun = sun;
    for (u32 i = 0; i < 2; i++) {
        auto inputs = Load<std::array<float, 7>>(view + bake_fields[i]);
        for (u32 k = 0; k < 6; k++) {
            fog.bakes[i][k] = inputs[k];
        }
        fog.bake_cumulus[i] = cumulus(static_cast<u32>(static_cast<s32>(inputs[6])));
        // The handle stands in for the index, so a texture that arrives late bakes again.
        inputs[6] = static_cast<float>(fog.bake_cumulus[i]);
        if (inputs != g_fog.maps[i]) {
            g_fog.maps[i] = inputs;
            g_fog.map_changes[i]++;
            g_fog.darkness_changes[i]++;
        } else if (sun_moved) {
            g_fog.darkness_changes[i]++;
        }
    }
    std::memcpy(fog.sun, sun.data(), sizeof(sun));
    std::memcpy(packet + Scene::FogOffset, &fog, sizeof(fog));
    FogHeader header{
        .flags = shadowed ? Scene::FogShadow : 0,
        .maps = g_fog.map_changes,
        .darkness = g_fog.darkness_changes,
    };
    for (u32 i = 0; i < 2; i++) {
        const VAddr memory = Load<VAddr>(frame_graph + FrameGraphFogNoise[i] + TextureRecordMemory);
        header.noise[i] = memory != 0 ? Load<VAddr>(memory) : 0;
    }
    return header;
}

// The first point light the capture copies is logged once, to check its fields.
bool g_logged_point_light = false;

// The values the game's LightCompute, LightPass, IBL and Resolve passes set up on the draw thread
// after the capture, from the view, and its point lights. The cascades' projections take view
// space to their texture coordinates. Returns the point lights copied.
u32 WriteLight(VAddr frame_graph, VAddr view, u8* packet) {
    Scene::Light light{};
    const auto number = [view](u64 field) { return Load<float>(view + field); };
    const auto copy = [view](float* out, u64 field, size_t bytes) {
        std::memcpy(out, reinterpret_cast<const void*>(view + field), bytes);
    };
    const auto inverse_view = Load<Matrix>(view + ViewInverse);
    for (u32 c = 0; c < Scene::Cascades; c++) {
        const Matrix projection = Multiply(
            inverse_view, Load<Matrix>(view + ViewCascades + c * CascadeBytes + CascadeTexture));
        const Matrix inverse = Invert(projection);
        std::memcpy(light.shadow_projection[c], projection.data(), sizeof(projection));
        std::memcpy(light.inverse_shadow_projection[c], inverse.data(), sizeof(inverse));
    }
    const Matrix view_to_sky = Multiply(inverse_view, Load<Matrix>(view + ViewSkyRotation));
    std::memcpy(light.view_to_sky, view_to_sky.data(), sizeof(view_to_sky));
    const Matrix view_to_previous =
        Multiply(inverse_view, Load<Matrix>(view + ViewPreviousViewProjection));
    std::memcpy(light.view_to_previous, view_to_previous.data(), sizeof(view_to_previous));
    light.depth_range = number(ViewDepthRange);
    copy(light.inverse_projection, ViewProjectionTerms, sizeof(light.inverse_projection));
    copy(light.split_depths, ViewSplitDepths, sizeof(light.split_depths));
    copy(light.light_color, ViewKeyLight, sizeof(light.light_color));
    copy(light.light_direction, ViewKeyLight + 0x10, sizeof(light.light_direction));
    const auto occluder_direction = Load<std::array<float, 3>>(view + ViewOccluderDirection);
    for (u32 j = 0; j < 3; j++) {
        light.occluder_direction[j] = -occluder_direction[j];
    }
    copy(light.gradation_high, ViewGradation, 3 * sizeof(float));
    copy(light.gradation_middle, ViewGradation + 12, 3 * sizeof(float));
    copy(light.gradation_low, ViewGradation + 24, 3 * sizeof(float));
    copy(light.contour_color, ViewContourColor, sizeof(light.contour_color));
    copy(light.specular_fade, ViewSpecularFade, sizeof(light.specular_fade));
    const auto inverse_length = [&number](u64 start, u64 end) {
        return 1.0f / std::max(number(end) - number(start), MinFadeDistance);
    };
    light.shadow_fade_start = number(ViewShadowEnd) - ShadowEndFade;
    light.ibl_intensity = number(ViewAmbient);
    light.contour_blend = number(ViewContourBlend);
    light.contour_distant_blend = number(ViewContourDistantBlend);
    light.contour_start = number(ViewContourStart);
    light.contour_range = inverse_length(ViewContourStart, ViewContourEnd);
    light.gradation_start = number(ViewGradationStart);
    light.gradation_range = inverse_length(ViewGradationStart, ViewGradationEnd);
    light.background_start = number(ViewBackgroundStart);
    light.background_range = inverse_length(ViewBackgroundStart, ViewBackgroundEnd);
    light.background_intensity = number(ViewBackgroundIntensity);
    light.background_lod = number(ViewBackgroundLod);
    light.hatching_intensity = 2.0f * number(ViewHatching);
    light.occlusion_intensity = OcclusionScale * number(ViewOcclusion);
    light.occlusion_select = number(ViewOcclusionSelect);
    light.toning = number(ViewToning) + 1.0f;
    const auto& occluder =
        Load<s32>(view + ViewOccluderMode) > 0 ? OccluderModeConstants : OccluderConstants;
    light.occluder_intensity = occluder[0];
    light.occluder_penumbra = occluder[1];
    light.occluder_range = occluder[2];
    const VAddr spheres = Load<VAddr>(view + ViewOccluders);
    light.occluders = spheres != 0 ? std::clamp<s32>(Load<s32>(view + ViewOccluderCount), 0,
                                                     static_cast<s32>(Scene::MaxOccluders))
                                   : 0;
    std::memcpy(light.occluder_spheres, reinterpret_cast<const void*>(spheres),
                light.occluders * sizeof(light.occluder_spheres[0]));
    const auto [first, second] =
        HeaviestTwo(Load<VAddr>(view + ViewAmbientLights), Load<s32>(view + ViewAmbientLightCount),
                    AmbientLightBytes, AmbientLightWeight, [](VAddr) { return true; });
    const auto cube = [](VAddr record, u64 field) {
        const VAddr object = record != 0 ? Load<VAddr>(record + field) : 0;
        return object != 0 ? Textures::GetCube(object) : 0;
    };
    light.radiance[0] = cube(first, AmbientLightRadiance);
    light.irradiance[0] = cube(first, AmbientLightIrradiance);
    light.radiance[1] = cube(second, AmbientLightRadiance);
    light.irradiance[1] = cube(second, AmbientLightIrradiance);
    if (second != 0) {
        const float second_weight = Load<float>(second + AmbientLightWeight);
        light.ibl_blend = second_weight / (second_weight + Load<float>(first + AmbientLightWeight));
    }
    std::memcpy(light.sh, reinterpret_cast<const void*>(frame_graph + FrameGraphSh),
                sizeof(light.sh));
    light.grid_scale[0] = number(ViewProjectionX);
    light.grid_scale[1] = number(ViewProjectionY);
    light.grid_start = number(ViewGridNear);
    light.shadow_influence = number(ViewShadowInfluence);
    const VAddr point_lights = Load<VAddr>(view + ViewPointLights);
    if (point_lights != 0) {
        light.point_lights = std::clamp<s32>(Load<s32>(view + ViewPointLightCount), 0,
                                             static_cast<s32>(Scene::MaxPointLights));
        std::memcpy(packet + Scene::PointLightsOffset, reinterpret_cast<const void*>(point_lights),
                    light.point_lights * sizeof(Scene::PointLight));
        if (light.point_lights != 0 && !g_logged_point_light) {
            g_logged_point_light = true;
            const auto first = Load<Scene::PointLight>(point_lights);
            LOG_INFO(Render_Vulkan,
                     "GR2port point lights: {} in the view, the first at ({:.2f} {:.2f} {:.2f}) "
                     "with range {:.2f}, colour ({:.2f} {:.2f} {:.2f}), falloff from {:.2f} by "
                     "power {:.2f}, shadow {:.2f}; shadow influence {:.2f}",
                     light.point_lights, first.position[0], first.position[1], first.position[2],
                     first.range, first.color[0], first.color[1], first.color[2],
                     first.falloff_start, first.falloff_power, first.shadow,
                     light.shadow_influence);
        }
    }
    std::memcpy(packet + Scene::LightOffset, &light, sizeof(light));
    return light.point_lights;
}

// The effector constants and spheres as the game's effector program reads them.
void WriteEffectors(VAddr view, u8* packet, Writer& writer) {
    auto* effectors = reinterpret_cast<Scene::Effectors*>(packet + Scene::EffectorsOffset);
    const float start = Load<float>(view + ViewGridNear);
    const u32 count = Load<u32>(view + ViewSphereCount);
    const VAddr spheres = Load<VAddr>(view + ViewSpheres);
    const u32 copied = spheres != 0 ? std::min(count, Scene::MaxSpheres) : 0;
    std::memcpy(effectors->view, reinterpret_cast<const void*>(view + ViewMatrix),
                sizeof(effectors->view));
    effectors->depth = GridFar - start;
    effectors->start = start;
    effectors->slice = (GridFar - start) * 0.1f;
    effectors->count = copied;
    effectors->projection_x = Load<float>(view + ViewProjectionX);
    effectors->projection_y = Load<float>(view + ViewProjectionY);
    std::memcpy(effectors->spheres, reinterpret_cast<const void*>(spheres), copied * SphereBytes);
    writer.SetSpheres(count);
}

GridConstants ReadGridConstants(VAddr view) {
    const float start = Load<float>(view + ViewGridNear);
    GridConstants grid{
        .matrix = Load<std::array<float, 16>>(view + ViewGridMatrix),
        .spheres = Load<u32>(view + ViewSphereCount),
        .fade = {(GridFadeStart - start) * (1.0f / (GridFar - start)), (GridFar - start) * 0.125f},
    };
    const float depth_scale = 1.0f / (start + GridFar);
    for (u32 column = 0; column < 4; column++) {
        grid.matrix[column * 4 + 2] *= depth_scale;
    }
    return grid;
}

} // namespace

// A written scene that waits for the particle manager's steps, later in the frame graph.
struct PendingPacket {
    VAddr view = 0;
    u8* packet = nullptr;
    u64 frame = 0;
    Scene::Header header{};
};
PendingPacket g_pending;

void PublishPending() {
    if (g_pending.packet != nullptr) {
        std::memcpy(g_pending.packet, &g_pending.header, sizeof(g_pending.header));
        SceneRing::Publish(g_pending.frame);
        g_pending.packet = nullptr;
    }
}

void LogFrameRate(std::chrono::steady_clock::time_point now, bool capturing) {
    if (g_rate_frames == 0) {
        g_rate_start = now;
    } else {
        const auto gap = now - g_rate_last;
        g_rate_longest = std::max(g_rate_longest, gap);
        g_rate_long_gaps += gap > LongGap ? 1 : 0;
    }
    g_rate_last = now;
    g_rate_frames++;
    if (now - g_rate_start < FrameRatePeriod) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - g_rate_start).count();
    const double rate = (g_rate_frames - 1) / seconds;
    const char* const renderer = capturing ? "on" : "off";
    const char* const fork = EmulatedGpu::On() ? "on" : "off";
    LOG_INFO(Render_Vulkan,
             "GR2port frame rate: {:.1f} game frames/s, longest gap {:.1f} ms, {} gaps over 50 ms; "
             "GR2port {}, GR2fork {}",
             rate, std::chrono::duration<double, std::milli>(g_rate_longest).count(),
             g_rate_long_gaps, renderer, fork);
    if (!capturing) {
        Window::SetTitle(fmt::format("GR2port off | game {:.0f} fps | GR2fork {}", rate, fork));
    }
    g_rate_frames = 0;
    g_rate_long_gaps = 0;
    g_rate_longest = {};
}

void OnFrame(VAddr frame_graph, VAddr view) {
    PublishPending();
    const auto start = std::chrono::steady_clock::now();
    const u64 frame = g_frames.load(std::memory_order_relaxed) + 1;
    g_frames.store(frame, std::memory_order_relaxed);
    const bool capturing = g_capturing.load(std::memory_order_relaxed);
    LogFrameRate(start, capturing);
    if (!capturing) {
        return;
    }
    u8* const packet = SceneRing::Begin();
    if (packet == nullptr) {
        return;
    }
    Writer writer{packet, SceneRing::Address(packet), frame, start};
    WriteShadows(view, packet, writer);
    WriteSky(frame_graph, view, packet, writer);
    WriteGrade(view, packet);
    const u32 point_lights = WriteLight(frame_graph, view, packet);
    const FogHeader fog = WriteFog(frame_graph, view, packet);
    const VAddr hatching = Load<VAddr>(frame_graph + FrameGraphHatching);
    writer.SetHatching(hatching != 0 ? Textures::Get(hatching) : 0);
    writer.EndPart(Scene::CapturePart::Constants);
    const VAddr records = Load<VAddr>(view + ViewDeformers);
    const VAddr list = Load<VAddr>(view + ViewDeformerList);
    if (records != 0 && list != 0) {
        const auto record = [records, list](u32 e) {
            return records + (static_cast<u64>(Load<u32>(list + e * 4) & 0xffff) << 6);
        };
        const auto range = [view](u32 kind) {
            return std::pair<u32, u32>{Load<u16>(view + ViewDeformerRanges + kind * 2),
                                       Load<u16>(view + ViewDeformerRanges + (kind + 1) * 2)};
        };
        const auto [first_skin, end_skin] = range(KindSkin);
        for (u32 e = first_skin; e < end_skin; e++) {
            writer.AddSkin(record(e));
        }
        const auto [first_soft_body, end_soft_body] = range(KindSoftBody);
        const auto [first_branch, end_branch] = range(KindBranch);
        const auto [first_grass, end_grass] = range(KindGrass);
        if (first_soft_body < end_soft_body || first_branch < end_branch ||
            first_grass < end_grass) {
            WriteEffectors(view, packet, writer);
        }
        if (first_soft_body < end_soft_body) {
            const GridConstants grid = ReadGridConstants(view);
            for (u32 e = first_soft_body; e < end_soft_body; e++) {
                writer.AddSoftBody(record(e), grid);
            }
        }
        for (u32 e = first_branch; e < end_branch; e++) {
            writer.AddBranchJob(record(e));
        }
        for (u32 e = first_grass; e < end_grass; e++) {
            writer.AddGrassRecord(record(e));
        }
    }
    writer.EndPart(Scene::CapturePart::Deformers);
    const VAddr items = Load<VAddr>(view + ViewItems);
    const VAddr keys = Load<VAddr>(view + ViewKeys);
    const s32 key_count = Load<s32>(view + ViewKeyCount);
    const auto keyed = [items, keys, key_count, view](u32 category) {
        const s32 first = Load<s32>(view + ViewCategoryFirst + category * 4);
        const VAddr key = keys + static_cast<s64>(first - key_count) * 4;
        return [items, key](s32 i) {
            return items + (static_cast<u64>(Load<u32>(key + i * 4) & 0x3fffff) << 6);
        };
    };
    for (const CasterCategory& category : CasterCategories) {
        const s32 count = Load<s32>(view + ViewCategoryCount + category.index * 4);
        ForEachItem(count, CasterWorld, None, keyed(category.index),
                    [&](VAddr item) { writer.AddCaster(item, category); });
    }
    ForEachItem(Load<s32>(view + ViewCategoryCount + LeafShadowCategory * 4), LeafDomainConstants,
                LeafPixelConstants, keyed(LeafShadowCategory),
                [&](VAddr item) { writer.AddLeafShadow(item); });
    writer.EndPart(Scene::CapturePart::Casters);
    const auto add_category = [&](const Category& category) {
        const s32 count = std::min(Load<s32>(view + ViewCategoryCount + category.index * 4),
                                   static_cast<s32>(Scene::MaxDraws));
        ForEachItem(count, category.constants, category.material, keyed(category.index),
                    [&](VAddr item) { writer.Add(item, category); });
    };
    add_category(MainCategory);
    const VAddr batch = Load<VAddr>(view + ViewBatchList);
    if (batch != 0) {
        const VAddr entries = Load<VAddr>(batch);
        const s32 count = std::min(Load<s32>(batch + 8), static_cast<s32>(Scene::MaxDraws));
        ForEachItem(
            count, MainCategory.constants, MainCategory.material,
            [items, entries](s32 i) {
                return items + static_cast<u32>(Load<u32>(entries + i * 8 + 4) << 6);
            },
            [&](VAddr item) { writer.Add(item, MainCategory); });
    }
    for (const Category& category : WorldCategories) {
        add_category(category);
    }
    const s32 metaball_items = std::min(Load<s32>(view + ViewCategoryCount + MetaballCategory * 4),
                                        static_cast<s32>(Scene::MaxMetaballs));
    if (metaball_items > 0) {
        const auto metaball_at = keyed(MetaballCategory);
        std::array<MetaballSphereOf, Scene::MaxMetaballs> spheres;
        for (s32 i = 0; i < metaball_items; i++) {
            const VAddr item = metaball_at(i);
            spheres[i] = {item, Load<std::array<float, 4>>(item + MetaballSphere)};
        }
        const std::span<const MetaballSphereOf> all{spheres.data(),
                                                    static_cast<size_t>(metaball_items)};
        ForEachItem(metaball_items, MetaballVertexConstants, MetaballPixelConstants, metaball_at,
                    [&](VAddr item) { writer.AddMetaball(item, all); });
    }
    add_category(GlassCategory);
    ForEachItem(Load<s32>(view + ViewCategoryCount + BranchCategory * 4), LeafDomainConstants,
                BranchPixelConstants, keyed(BranchCategory),
                [&](VAddr item) { writer.AddBranch(item); });
    ForEachItem(Load<s32>(view + ViewCategoryCount + LeafCategory * 4), LeafDomainConstants,
                LeafPixelConstants, keyed(LeafCategory), [&](VAddr item) { writer.AddLeaf(item); });
    const s32 grass_items = Load<s32>(view + ViewCategoryCount + GrassCategory * 4);
    if (grass_items > 0) {
        const GrassView grass_view{
            .view = Load<Matrix>(view + ViewMatrix),
            .view_projection = Load<Matrix>(view + ViewGridMatrix),
            .cluster_scale = 1.0f / (Load<float>(view + ViewGridNear) + GridFar),
            .time_step = Load<float>(view + ViewTimeStep),
        };
        ForEachItem(grass_items, GrassGeometryConstants, GrassPixelConstants, keyed(GrassCategory),
                    [&](VAddr item) { writer.AddGrass(item, grass_view); });
        writer.AdvanceGrassPhase();
    }
    const u32 world_decals = std::min(Load<u32>(view + ViewWorldDecalCount), Scene::MaxWorldDecals);
    const VAddr decal_records = Load<VAddr>(view + ViewWorldDecals);
    if (world_decals != 0 && decal_records != 0) {
        const DecalView decal_view{
            .view = Load<Matrix>(view + ViewMatrix),
            .inverse_view = Load<Matrix>(view + ViewInverse),
            .projection = Load<Matrix>(view + ViewProjection),
        };
        for (u32 i = 0; i < world_decals; i++) {
            writer.AddWorldDecal(decal_records + i * WorldDecalBytes, decal_view);
        }
    }
    writer.EndPart(Scene::CapturePart::World);
    add_category(CharacterCategory);
    add_category(SelfTranslucentCategory);
    add_category(InteriorCategory);
    ForEachItem(Load<s32>(view + ViewCategoryCount + ContourCategory * 4), ContourVertexConstants,
                None, keyed(ContourCategory), [&](VAddr item) { writer.AddContour(item); });
    writer.AddEffects(view);
    ForEachItem(std::min(Load<s32>(view + ViewCategoryCount + WaterCategory * 4),
                         static_cast<s32>(Scene::MaxWaterDraws)),
                WaterVertexConstants, WaterPixelConstants, keyed(WaterCategory),
                [&](VAddr item) { writer.AddWater(item, view); });
    writer.CopyCommands();
    writer.EndPart(Scene::CapturePart::Characters);
    Scene::Header header = writer.Header();
    header.frame = frame;
    header.frame_gap_ns = static_cast<u32>(std::min<s64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(start - g_last_frame_start).count(),
        std::numeric_limits<u32>::max()));
    g_last_frame_start = start;
    header.point_lights = point_lights;
    header.fog = fog.flags;
    for (u32 i = 0; i < 2; i++) {
        header.fog_maps[i] = fog.maps[i];
        header.fog_darkness[i] = fog.darkness[i];
        header.fog_noise[i] = fog.noise[i];
    }
    header.bloom = (Load<float>(view + ViewBloomStrength) != 0.0f ? Scene::BloomOn : 0) |
                   (Load<float>(view + ViewBloomFeedback) > 0.0f ? Scene::BloomFeedback : 0);
    for (u32 c = 0; c < Scene::ItemCategories; c++) {
        const s32 count = Load<s32>(view + ViewCategoryCount + c * 4);
        header.category_items[c] = static_cast<u16>(std::clamp(count, 0, 0xffff));
    }
    header.capture_ns = static_cast<u32>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    g_pending.packet = packet;
    g_pending.view = view;
    g_pending.frame = frame;
    g_pending.header = header;
}

void OnParticleSetup(VAddr fx) {
    ParticleCapture::OnSetup(fx);
}

void OnParticleList(VAddr fx) {
    if (g_pending.packet != nullptr) {
        const auto start = std::chrono::steady_clock::now();
        ParticleCapture::Write(fx, g_pending.view, g_pending.packet, g_pending.header);
        g_pending.header.particles.capture_ns =
            static_cast<u32>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count());
        PublishPending();
    }
}

u64 Frames() {
    return g_frames.load(std::memory_order_relaxed);
}

void ToggleCapture() {
    g_capturing.store(!g_capturing.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

} // namespace Gr2Port::Capture
