// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

#include "video_core/gr2port/capture.h"
#include "video_core/gr2port/deformers.h"
#include "video_core/gr2port/meshes.h"
#include "video_core/gr2port/scene.h"
#include "video_core/gr2port/scene_ring.h"
#include "video_core/gr2port/textures.h"

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
// View: V +0x20, inverse(V) +0x60, the cascade records from +0x400 every 0x110 ({valid, split,
// depth bias constant, depth bias slope} and the cascade VP at +0x90), and the casters'
// view-projection tables from +0x748 (a pointer per table and cascade).
constexpr u64 ViewMatrix = 0x20;
constexpr u64 ViewInverse = 0x60;
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
// The game's Composite pass takes these from a view's values.
constexpr u32 CurveKnots = 7;
constexpr float LensOutwardScale = 1.3f;
constexpr float LensOutwardShape = 1.4281480f; // tan(55 degrees)
constexpr float LensInwardBend = 6.0f;
// The game's adaption rate counts seconds at 30 frames per second.
constexpr float AdaptionFrame = 1.0f / 30.0f;
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
};
constexpr std::array<Category, 8> Categories{{
    {15, 0x60, 0x38, 0x68, 0x00, 0x20},      // G2_BG_Main
    {16, 0x38, 0x48, 0x40, 0x80, 0x00},      // BG layer: the first of four layers
    {17, 0x38, 0x48, 0x40, 0x00, None},      // BG single
    {19, 0x38, 0x48, 0x40, 0x00, None},      // BG gem
    {21, 0x38, 0x48, 0x40, 0x00, None},      // BG interior
    {22, 0x38, 0x48, 0x40, 0x30, 0x00},      // BG vein
    {23, 0x30, None, None, None, None},      // Far: untextured
    {8, 0x68, 0x30, 0x70, 0xa0, None, 0x50}, // G2_CH_Main: colour texture t1, decal texture t6
}};
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

// The simulate constants the game's SoftBody pass sets on its draw thread after the capture: the
// grid matrix (the view's clip matrix with z over GridFar + n), the sphere count and the depth
// fade.
struct GridConstants {
    std::array<float, 16> matrix;
    u32 spheres;
    std::array<float, 2> fade;
};

template <typename T>
T Load(VAddr address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

// Writes the draws of a packet. The packet's memory is write-combined, where reads are slow:
// the writer only stores to it, and counts in its own members.
class Writer {
public:
    /// packet is at address on the GPU; frame is the capture's.
    Writer(u8* packet, u64 address, u64 frame)
        : frame{frame}, draws{reinterpret_cast<Scene::Draw*>(packet + Scene::DrawsOffset)},
          commands{reinterpret_cast<Scene::Command*>(packet + Scene::CommandsOffset)},
          casters{reinterpret_cast<Scene::Caster*>(packet + Scene::CastersOffset)},
          caster_commands{reinterpret_cast<Scene::Command*>(packet + Scene::CasterCommandsOffset)},
          palettes{packet + Scene::PalettesOffset},
          palettes_address{address + Scene::PalettesOffset},
          skin_jobs{reinterpret_cast<Scene::SkinJob*>(packet + Scene::SkinJobsOffset)},
          soft_body_jobs{reinterpret_cast<Scene::SoftBodyJob*>(packet + Scene::SoftBodyJobsOffset)},
          decals{reinterpret_cast<Scene::Decal*>(packet + Scene::DecalsOffset)} {}

    void SetCascades(u32 mask) {
        header.cascades = mask;
    }

    void SetSpheres(u32 count) {
        header.spheres = count;
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
        commands[i] = {
            .index_count = count,
            .instance_count = 1,
            .first_index = mesh->first_index + first,
            .vertex_offset = 0,
            .first_instance = 0,
        };
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
            caster_commands[(c * Scene::CasterLists + list) * Scene::MaxCasters + n++] = {
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

    Scene::Header header{};
    u64 frame;
    Scene::Draw* draws;
    Scene::Command* commands;
    Scene::Caster* casters;
    Scene::Command* caster_commands;
    u8* palettes;
    u64 palettes_address;
    Scene::SkinJob* skin_jobs;
    Scene::SoftBodyJob* soft_body_jobs;
    Scene::Decal* decals;
    VAddr last_palette = 0;
    u32 last_bones = 0;
    u64 last_address = 0;
};

// Items are scattered through the pool and point at scattered objects. The loops prefetch the
// item Lookahead places ahead, and the objects of the item half as far ahead.
constexpr s32 Lookahead = 8;

void Prefetch(VAddr address) {
    __builtin_prefetch(reinterpret_cast<const void*>(address));
}

// The submesh, the shape's handle (just before its index buffer object) and the matrices of an
// item.
void PrefetchObjects(VAddr item, u64 matrices) {
    const VAddr submesh = Load<VAddr>(item + ItemSubmesh);
    Prefetch(submesh);
    Prefetch(submesh + SubmeshRange);
    Prefetch(Load<VAddr>(item + ItemIndexBuffer) - 4);
    Prefetch(Load<VAddr>(item + matrices));
}

template <typename ItemAt, typename Visit>
void ForEachItem(s32 count, u64 matrices, ItemAt item_at, Visit visit) {
    for (s32 i = 0; i < count; i++) {
        if (i + Lookahead < count) {
            Prefetch(item_at(i + Lookahead));
        }
        if (i + Lookahead / 2 < count) {
            PrefetchObjects(item_at(i + Lookahead / 2), matrices);
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

using Matrix = std::array<float, 16>;

// a * b for the game's row-major matrices.
Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (u32 row = 0; row < 4; row++) {
        for (u32 column = 0; column < 4; column++) {
            for (u32 i = 0; i < 4; i++) {
                result[row * 4 + column] += a[row * 4 + i] * b[i * 4 + column];
            }
        }
    }
    return result;
}

// The sky of the game's sky pass: the two heaviest skies of the view's list with a cube map,
// blended by weight. With no sky the pass fills the scene with the background colour.
void WriteSky(VAddr frame_graph, VAddr view, u8* packet, Writer& writer) {
    const auto background = Load<std::array<float, 3>>(view + ViewBackground);
    const VAddr list = Load<VAddr>(view + ViewSkies);
    const s32 count = Load<s32>(view + ViewSkyCount);
    VAddr first = 0;
    VAddr second = 0;
    for (s32 i = 0; i < count; i++) {
        const VAddr sky = list + i * SkyBytes;
        if (Load<VAddr>(sky + SkyCube) == 0) {
            continue;
        }
        const float weight = Load<float>(sky + SkyWeight);
        if (first == 0) {
            first = sky;
        } else if (weight > Load<float>(first + SkyWeight)) {
            second = first;
            first = sky;
        } else if (second == 0 || weight > Load<float>(second + SkyWeight)) {
            second = sky;
        }
    }
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
    std::memcpy(packet + Scene::GradeOffset, &grade, sizeof(grade));
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

void OnFrame(VAddr frame_graph, VAddr view) {
    const auto start = std::chrono::steady_clock::now();
    const u64 frame = g_frames.load(std::memory_order_relaxed) + 1;
    g_frames.store(frame, std::memory_order_relaxed);
    u8* const packet = SceneRing::Begin();
    if (packet == nullptr) {
        return;
    }
    Writer writer{packet, SceneRing::Address(packet), frame};
    WriteShadows(view, packet, writer);
    WriteSky(frame_graph, view, packet, writer);
    WriteGrade(view, packet);
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
        if (first_soft_body < end_soft_body) {
            WriteEffectors(view, packet, writer);
            const GridConstants grid = ReadGridConstants(view);
            for (u32 e = first_soft_body; e < end_soft_body; e++) {
                writer.AddSoftBody(record(e), grid);
            }
        }
    }
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
        ForEachItem(count, CasterWorld, keyed(category.index),
                    [&](VAddr item) { writer.AddCaster(item, category); });
    }
    for (const Category& category : Categories) {
        const s32 count = std::min(Load<s32>(view + ViewCategoryCount + category.index * 4),
                                   static_cast<s32>(Scene::MaxDraws));
        ForEachItem(count, category.constants, keyed(category.index),
                    [&](VAddr item) { writer.Add(item, category); });
    }
    const VAddr batch = Load<VAddr>(view + ViewBatchList);
    if (batch != 0) {
        const VAddr entries = Load<VAddr>(batch);
        const s32 count = std::min(Load<s32>(batch + 8), static_cast<s32>(Scene::MaxDraws));
        ForEachItem(
            count, Categories[0].constants,
            [items, entries](s32 i) {
                return items + static_cast<u32>(Load<u32>(entries + i * 8 + 4) << 6);
            },
            [&](VAddr item) { writer.Add(item, Categories[0]); });
    }
    Scene::Header header = writer.Header();
    header.frame = frame;
    for (u32 c = 0; c < Scene::ItemCategories; c++) {
        const s32 count = Load<s32>(view + ViewCategoryCount + c * 4);
        header.category_items[c] = static_cast<u16>(std::clamp(count, 0, 0xffff));
    }
    header.capture_ns = static_cast<u32>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    std::memcpy(packet, &header, sizeof(header));
    SceneRing::Publish(frame);
}

u64 Frames() {
    return g_frames.load(std::memory_order_relaxed);
}

} // namespace Gr2Port::Capture
