// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <immintrin.h>

#include "common/logging/log.h"
#include "graphics/gr2port/device_lost.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/scene.h"

namespace Gr2Port::DeviceLost {

namespace {

constexpr std::array<const char*, static_cast<u32>(Pass::Count)> PassNames{
    "frame start",      "deformers",    "kernels",
    "simulation",       "point lights", "mesh sampling",
    "self translucent", "shadows",      "fog volumes",
    "surfaces",         "lines",        "character depth",
    "occlusion",        "velocity",     "light",
    "characters",       "sky",          "contours",
    "effects behind",   "glass",        "water",
    "effects in front", "fog apply",    "particles",
    "effects top-most", "meter",        "bloom",
    "antialias",        "composite",    "ui",
    "present",
};

// A marker holds the frame above the pass.
constexpr u32 PassBits = 8;
// The frames whose notes are kept: those in flight and the ones before them.
constexpr u32 KeptFrames = 4;
constexpr u32 MaxMarks = 48;
// Freed buffers the report still names.
constexpr u32 KeptFreed = 16384;
// Bad addresses logged in full; the rest are counted.
constexpr u32 LoggedAddresses = 32;
constexpr std::string_view LossLine = "GR2port: the device is lost";
constexpr u64 NoGuard = ~u64{0};
constexpr u32 MaxFields = 16;

struct FrameNote {
    u64 frame = 0;
    u32 marks = 0;
    std::array<u8, MaxMarks> passes{};
    // The frame's packet, which stays as it is while the frame is in flight, and a copy of its
    // start.
    const u8* packet = nullptr;
    Scene::Header header{};
};

struct Checked {
    u64 addresses = 0;
    u64 bad = 0;
};

struct BufferNote {
    u64 address = 0;
    u64 size = 0;
    const char* file = "";
    u32 line = 0;
    u64 freed = 0;
};

// A live buffer's addresses.
struct Range {
    u64 address = 0;
    u64 size = 0;
};

// A device address in a record: its byte offset and, for one read only when a count in the
// record is not 0, that count's offset.
struct Field {
    std::string_view name;
    u64 offset;
    u64 guard = NoGuard;
};

std::atomic<u64> g_frame{0};
bool g_reported = false;
bool g_armed = false;
std::string g_reason;
// GR2PORT_CHECK_ADDRESSES in the environment checks every frame's addresses as it is recorded.
bool g_check_frames = false;
u32 g_frames_in_flight = 1;
Memory* g_memory = nullptr;
Buffer g_crumbs;
std::array<FrameNote, KeptFrames> g_frames;
std::chrono::steady_clock::time_point g_submitted;
u64 g_submitted_frame = 0;

std::mutex g_buffers_mutex;
std::map<u64, BufferNote> g_buffers;
std::array<BufferNote, KeptFreed> g_freed;
u64 g_freed_count = 0;
// The live buffers in address order, for the trace's checks; made again after a change.
std::vector<Range> g_ranges;
bool g_ranges_stale = true;

u32 g_logged_addresses = 0;
bool g_logged_check = false;

std::string Where(const BufferNote& note) {
    return fmt::format("{}:{}", std::filesystem::path{note.file}.filename().string(), note.line);
}

// Whether address lies in a live buffer: the one hit holds, else one of g_ranges, which hit
// then holds. The caller holds the buffers' mutex.
bool Live(u64 address, Range& hit) {
    if (address - hit.address < hit.size) {
        return true;
    }
    const auto after =
        std::upper_bound(g_ranges.begin(), g_ranges.end(), address,
                         [](u64 value, const Range& range) { return value < range.address; });
    if (after == g_ranges.begin()) {
        return false;
    }
    const Range& below = *std::prev(after);
    if (address - below.address >= below.size) {
        return false;
    }
    hit = below;
    return true;
}

// A value of a packet, which is in write-combined memory: a streaming load reads the 16 bytes
// that hold it far faster than a plain load does.
template <typename T>
T Load(const u8* record, u64 offset) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(record) + offset;
    alignas(16) std::array<u8, 16> block;
    _mm_store_si128(reinterpret_cast<__m128i*>(block.data()),
                    _mm_stream_load_si128(reinterpret_cast<__m128i*>(at & ~uintptr_t{15})));
    T value;
    std::memcpy(&value, block.data() + (at & 15), sizeof(value));
    return value;
}

// The live buffer address lies in, else the freed one, else the live one that ends below it.
// The caller holds the buffers' mutex.
std::string Describe(u64 address) {
    const auto after = g_buffers.upper_bound(address);
    const BufferNote* below = after == g_buffers.begin() ? nullptr : &std::prev(after)->second;
    if (below != nullptr && address - below->address < below->size) {
        return fmt::format("{} bytes into the {}-byte buffer of {}", address - below->address,
                           below->size, Where(*below));
    }
    const u64 kept = std::min<u64>(g_freed_count, KeptFreed);
    for (u64 i = 0; i < kept; i++) {
        const BufferNote& freed = g_freed[(g_freed_count - 1 - i) % KeptFreed];
        if (address >= freed.address && address - freed.address < freed.size) {
            return fmt::format("{} bytes into a {}-byte buffer of {} that frame {} freed",
                               address - freed.address, freed.size, Where(freed), freed.freed);
        }
    }
    if (below != nullptr) {
        return fmt::format("in no buffer, {} bytes past the end of the {}-byte buffer of {}",
                           address - below->address - below->size, below->size, Where(*below));
    }
    return "in no buffer, below all of them";
}

// Checks the device addresses at fields in count records of stride bytes. The caller holds the
// buffers' mutex.
void CheckList(const FrameNote& note, Checked& checked, std::string_view name, const u8* list,
               u32 count, u64 stride, std::span<const Field> fields) {
    // Each field's addresses mostly stay in the buffer that held the last one.
    std::array<Range, MaxFields> hits{};
    for (u32 i = 0; i < count; i++) {
        const u8* const record = list + i * stride;
        for (size_t f = 0; f < fields.size(); f++) {
            const Field& field = fields[f];
            if (field.guard != NoGuard && Load<u32>(record, field.guard) == 0) {
                continue;
            }
            const u64 address = Load<u64>(record, field.offset);
            if (address == 0) {
                continue;
            }
            checked.addresses++;
            if (Live(address, hits[f])) {
                continue;
            }
            checked.bad++;
            if (g_logged_addresses++ < LoggedAddresses) {
                LOG_ERROR(Render_Vulkan,
                          "GR2port: frame {} (game frame {}): {} {} of {}: {} {:#x} is {}",
                          note.frame, note.header.frame, name, i, count, field.name, address,
                          Describe(address));
            }
        }
    }
}

// The three addresses of a mesh at offset in a record; skin and palette are read under guard.
constexpr std::array<Field, 3> MeshFields(u64 offset, u64 vertices, u64 skin, u64 palette,
                                          u64 guard = NoGuard) {
    return {Field{"vertices", offset + vertices}, Field{"skin", offset + skin, guard},
            Field{"palette", offset + palette, guard}};
}

// Checks every device address of the records of note's packet against the live buffers.
Checked CheckPacket(const FrameNote& note) {
    using namespace Scene;
    const Header& header = note.header;
    const u8* const packet = note.packet;
    static constexpr auto draw = MeshFields(0, offsetof(Draw, vertices), offsetof(Draw, skin),
                                            offsetof(Draw, palette), offsetof(Draw, bones));
    static constexpr auto caster = MeshFields(0, offsetof(Caster, vertices), offsetof(Caster, skin),
                                              offsetof(Caster, palette), offsetof(Caster, bones));
    static constexpr auto contour =
        MeshFields(0, offsetof(Contour, vertices), offsetof(Contour, skin),
                   offsetof(Contour, palette), offsetof(Contour, bones));
    static constexpr auto effect =
        MeshFields(offsetof(EffectDraw, mesh), offsetof(EffectMesh, vertices),
                   offsetof(EffectMesh, skin), offsetof(EffectMesh, palette));
    static constexpr auto water =
        MeshFields(offsetof(WaterDraw, mesh), offsetof(EffectMesh, vertices),
                   offsetof(EffectMesh, skin), offsetof(EffectMesh, palette));
    static constexpr auto metaball =
        MeshFields(offsetof(MetaballDraw, mesh), offsetof(EffectMesh, vertices),
                   offsetof(EffectMesh, skin), offsetof(EffectMesh, palette));
    static constexpr auto enemy =
        MeshFields(offsetof(EnemyDraw, mesh), offsetof(EnemyMesh, vertices),
                   offsetof(EnemyMesh, skin), offsetof(EnemyMesh, palette));
    static constexpr auto glass =
        MeshFields(offsetof(CharacterGlassDraw, mesh), offsetof(EnemyMesh, vertices),
                   offsetof(EnemyMesh, skin), offsetof(EnemyMesh, palette));
    static constexpr std::array skin_job{
        Field{"source", offsetof(SkinJob, source)},
        Field{"destination", offsetof(SkinJob, destination)},
        Field{"skin", offsetof(SkinJob, skin)},
        Field{"palette", offsetof(SkinJob, palette)},
    };
    static constexpr std::array soft_body_job{
        Field{"skinned", offsetof(SoftBodyJob, skinned)},
        Field{"particles", offsetof(SoftBodyJob, particles)},
        Field{"constraints", offsetof(SoftBodyJob, constraints)},
        Field{"map", offsetof(SoftBodyJob, map)},
        Field{"state", offsetof(SoftBodyJob, state)},
        Field{"destination", offsetof(SoftBodyJob, destination)},
    };
    static constexpr std::array leaf{Field{"vertices", offsetof(Leaf, vertices)}};
    static constexpr std::array grass{Field{"tufts", offsetof(Grass, tufts)}};
    static constexpr std::array branch_job{
        Field{"source", offsetof(BranchJob, source)},
        Field{"destination", offsetof(BranchJob, destination)},
        Field{"state", offsetof(BranchJob, state)},
    };
    static constexpr std::array branch{Field{"vertices", offsetof(Branch, vertices)}};
    static constexpr std::array sample{
        Field{"vertices", offsetof(ParticleSample, vertices)},
        Field{"weights", offsetof(ParticleSample, weights)},
    };
    static constexpr auto ui_draw = [] {
        std::array<Field, 4 + UiAttributes> fields{
            Field{"indices", offsetof(UiDraw, indices)},
            Field{"vertex constants", offsetof(UiDraw, vs_constants)},
            Field{"vertex palette", offsetof(UiDraw, vs_palette)},
            Field{"pixel constants", offsetof(UiDraw, ps_constants)},
        };
        for (u32 i = 0; i < UiAttributes; i++) {
            fields[4 + i] = {"attribute", offsetof(UiDraw, attributes) + i * sizeof(UiAttribute) +
                                              offsetof(UiAttribute, address)};
        }
        return fields;
    }();

    Checked checked;
    {
        const std::scoped_lock lock{g_buffers_mutex};
        if (g_ranges_stale) {
            g_ranges_stale = false;
            g_ranges.clear();
            for (const auto& [address, note] : g_buffers) {
                g_ranges.push_back({address, note.size});
            }
        }
        const auto check = [&](std::string_view name, u64 offset, u32 count, u32 room, u64 stride,
                               std::span<const Field> fields) {
            CheckList(note, checked, name, packet + offset, std::min(count, room), stride, fields);
        };
        if (header.ui.only == 0) {
            check("draw", DrawsOffset, header.draws, g_room.draws, sizeof(Draw), draw);
            check("caster", CastersOffset(), header.casters, g_room.casters, sizeof(Caster),
                  caster);
            check("skin job", SkinJobsOffset, header.skin_jobs, MaxSkinJobs, sizeof(SkinJob),
                  skin_job);
            check("soft body job", SoftBodyJobsOffset, header.soft_body_jobs, MaxSoftBodyJobs,
                  sizeof(SoftBodyJob), soft_body_job);
            check("contour", ContoursOffset, header.contours, MaxContours, sizeof(Contour),
                  contour);
            check("leaf", LeavesOffset, header.leaves, MaxLeaves, sizeof(Leaf), leaf);
            check("grass", GrassOffset, header.grass, MaxGrass, sizeof(Grass), grass);
            check("branch job", BranchJobsOffset, header.branch_jobs, MaxBranchJobs,
                  sizeof(BranchJob), branch_job);
            check("branch", BranchesOffset, header.branches, MaxBranches, sizeof(Branch), branch);
            check("effect draw", EffectDrawsOffset, header.effects.draws, MaxEffectDraws,
                  sizeof(EffectDraw), effect);
            check("water draw", WaterDrawsOffset, header.effects.water_draws, MaxWaterDraws,
                  sizeof(WaterDraw), water);
            check("metaball", MetaballDrawsOffset, header.metaballs, MaxMetaballs,
                  sizeof(MetaballDraw), metaball);
            check("enemy", EnemyDrawsOffset, header.enemies, MaxEnemyDraws, sizeof(EnemyDraw),
                  enemy);
            check("character glass", CharacterGlassDrawsOffset, header.character_glass,
                  MaxCharacterGlassDraws, sizeof(CharacterGlassDraw), glass);
            check("particle sample", ParticleSamplesOffset, header.particles.samples,
                  MaxParticleSamples, sizeof(ParticleSample), sample);
        }
        check("screen draw", UiDrawsOffset, header.ui.draws, MaxUiDraws, sizeof(UiDraw), ui_draw);
    }
    return checked;
}

void ReportFrames(u64 frame) {
    for (u64 f = frame < KeptFrames ? 1 : frame - KeptFrames + 1; f <= frame; f++) {
        const FrameNote& note = g_frames[f % KeptFrames];
        if (note.frame != f) {
            continue;
        }
        const Scene::Header& header = note.header;
        LOG_ERROR(Render_Vulkan,
                  "GR2port: frame {} drew game frame {}: {}, {} draws, {} casters, {} indices, {} "
                  "passes",
                  f, header.frame, header.ui.only == 0 ? "the world" : "the screen alone",
                  header.draws, header.casters, header.indices, note.marks);
    }
}

// The frames in flight still hold their packets: what each draws, and its records' device
// addresses checked against the buffers.
void ReportPackets(u64 frame) {
    g_logged_addresses = 0;
    for (u64 f = frame < g_frames_in_flight ? 1 : frame - g_frames_in_flight + 1; f <= frame; f++) {
        const FrameNote& note = g_frames[f % KeptFrames];
        if (note.frame != f || note.packet == nullptr) {
            continue;
        }
        const Scene::Header& header = note.header;
        LOG_ERROR(Render_Vulkan,
                  "GR2port: frame {} holds {} skin jobs, {} soft bodies, {} outlines, {} leaves, "
                  "{} grass, {} branch jobs, {} branches, {} effect draws, {} water draws, {} "
                  "metaballs, {} enemies, {} character glass, {} particle draws of {} live effects "
                  "(library {}), {} mesh samples, {} point lights, {} screen draws",
                  f, header.skin_jobs, header.soft_body_jobs, header.contours, header.leaves,
                  header.grass, header.branch_jobs, header.branches, header.effects.draws,
                  header.effects.water_draws, header.metaballs, header.enemies,
                  header.character_glass, header.particles.draws, header.particles.live_effects,
                  header.particles.generation, header.particles.samples, header.point_lights,
                  header.ui.draws);
        const Checked checked = CheckPacket(note);
        LOG_ERROR(Render_Vulkan,
                  "GR2port: frame {}: {} device addresses of its records checked against the "
                  "buffers, {} bad",
                  f, checked.addresses, checked.bad);
    }
}

void ReportMemory() {
    if (g_memory == nullptr) {
        return;
    }
    const Memory::Budget budget = g_memory->DeviceBudget();
    const std::scoped_lock lock{g_buffers_mutex};
    LOG_ERROR(Render_Vulkan,
              "GR2port: GPU memory at the last frame: {} MB in use of a budget of {} MB, {} MB "
              "of it the renderer's; {} buffers live, {} freed so far",
              budget.usage >> 20, budget.budget >> 20, budget.own >> 20, g_buffers.size(),
              g_freed_count);
}

void ReportCheckpoint(u64 stage, const void* checkpoint) {
    const u64 marker = reinterpret_cast<u64>(checkpoint);
    const u64 pass = marker & ((u64{1} << PassBits) - 1);
    LOG_ERROR(Render_Vulkan, "GR2port: stage {:#x} reached frame {}, {}", stage, marker >> PassBits,
              pass < PassNames.size() ? PassNames[pass] : "nothing");
}

void ReportCheckpoints(const Device& d) {
    const auto& dl = d.dispatch;
    if (!d.checkpoints ||
        (dl.vkGetQueueCheckpointData2NV == nullptr && dl.vkGetQueueCheckpointDataNV == nullptr)) {
        LOG_ERROR(Render_Vulkan, "GR2port: no checkpoints on this GPU");
        return;
    }
    u32 count = 0;
    // Some drivers have the first call alone.
    if (dl.vkGetQueueCheckpointData2NV == nullptr) {
        dl.vkGetQueueCheckpointDataNV(d.queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> points(count, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        dl.vkGetQueueCheckpointDataNV(d.queue, &count, points.data());
        LOG_ERROR(Render_Vulkan, "GR2port: {} checkpoints", count);
        for (u32 i = 0; i < count; i++) {
            ReportCheckpoint(static_cast<u64>(points[i].stage), points[i].pCheckpointMarker);
        }
        return;
    }
    dl.vkGetQueueCheckpointData2NV(d.queue, &count, nullptr);
    std::vector<VkCheckpointData2NV> points(count, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV});
    dl.vkGetQueueCheckpointData2NV(d.queue, &count, points.data());
    LOG_ERROR(Render_Vulkan, "GR2port: {} checkpoints", count);
    for (u32 i = 0; i < count; i++) {
        ReportCheckpoint(points[i].stage, points[i].pCheckpointMarker);
    }
}

void ReportFault(const Device& d) {
    const auto& dl = d.dispatch;
    if (!d.device_fault || dl.vkGetDeviceFaultInfoEXT == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port: no fault information on this GPU");
        return;
    }
    VkDeviceFaultCountsEXT counts{.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (dl.vkGetDeviceFaultInfoEXT(d.device, &counts, nullptr) != VK_SUCCESS) {
        LOG_ERROR(Render_Vulkan, "GR2port: the fault's counts failed");
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
        .pAddressInfos = addresses.data(),
        .pVendorInfos = vendors.data(),
    };
    // The vendor's binary is not asked for, so a driver that has one answers incomplete and
    // still fills in the rest.
    const u64 binary_bytes = counts.vendorBinarySize;
    counts.vendorBinarySize = 0;
    if (const VkResult result = dl.vkGetDeviceFaultInfoEXT(d.device, &counts, &info);
        result != VK_SUCCESS && result != VK_INCOMPLETE) {
        LOG_ERROR(Render_Vulkan, "GR2port: the fault's information failed: {}",
                  vk::to_string(static_cast<vk::Result>(result)));
        return;
    }
    addresses.resize(std::min<size_t>(addresses.size(), counts.addressInfoCount));
    vendors.resize(std::min<size_t>(vendors.size(), counts.vendorInfoCount));
    LOG_ERROR(Render_Vulkan,
              "GR2port: fault \"{}\", {} addresses, {} vendor records, a vendor binary of {} bytes",
              info.description, addresses.size(), vendors.size(), binary_bytes);
    {
        const std::scoped_lock lock{g_buffers_mutex};
        for (const VkDeviceFaultAddressInfoEXT& address : addresses) {
            // The other kinds are places in the GPU's own program.
            const bool data =
                address.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT ||
                address.addressType == VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT;
            LOG_ERROR(
                Render_Vulkan, "GR2port: fault address {:#x}, within {:#x}, {}{}{}",
                address.reportedAddress, address.addressPrecision,
                vk::to_string(static_cast<vk::DeviceFaultAddressTypeEXT>(address.addressType)),
                data ? ": " : "", data ? Describe(address.reportedAddress) : "");
        }
    }
    for (const VkDeviceFaultVendorInfoEXT& vendor : vendors) {
        LOG_ERROR(Render_Vulkan, "GR2port: fault record \"{}\", code {:#x}, data {:#x}",
                  vendor.description, vendor.vendorFaultCode, vendor.vendorFaultData);
    }
}

// The passes the GPU wrote: each holds the low half of the last frame that began it.
void ReportCrumbs(u64 frame) {
    if (!g_armed) {
        LOG_ERROR(Render_Vulkan,
                  "GR2port: the GPU trace was off; the next start turns it on, and its log names "
                  "the pass the GPU was in");
        return;
    }
    std::array<u32, static_cast<u32>(Pass::Count)> crumbs{};
    std::memcpy(crumbs.data(), g_crumbs.mapped, sizeof(crumbs));
    const u32 begun = crumbs[static_cast<u32>(Pass::FrameStart)];
    std::string table;
    for (u32 pass = 0; pass < crumbs.size(); pass++) {
        table += fmt::format("{}{} {}", pass == 0 ? "" : ", ", PassNames[pass], crumbs[pass]);
    }
    LOG_ERROR(Render_Vulkan, "GR2port trace: the last frame that began each pass: {}", table);
    const FrameNote* note = nullptr;
    for (const FrameNote& kept : g_frames) {
        if (kept.marks != 0 && static_cast<u32>(kept.frame) == begun) {
            note = &kept;
        }
    }
    if (note == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port trace: the GPU began frame {} last, of {} recorded",
                  begun, frame);
    } else {
        u32 reached = 0;
        while (reached < note->marks && crumbs[note->passes[reached]] == begun) {
            reached++;
        }
        if (reached == note->marks) {
            LOG_ERROR(Render_Vulkan,
                      "GR2port trace: the GPU began all {} passes of frame {}, the last recorded "
                      "is {}; its last pass is {}",
                      note->marks, note->frame, frame, PassNames[note->passes[reached - 1]]);
        } else {
            LOG_ERROR(Render_Vulkan,
                      "GR2port trace: the GPU stopped in frame {} (the last recorded is {}), in "
                      "the pass {}: it began {} of the frame's {} passes and never began {}",
                      note->frame, frame, PassNames[note->passes[reached - 1]], reached,
                      note->marks, PassNames[note->passes[reached]]);
        }
    }
}

} // namespace

void Arm(std::string_view reason) {
    g_armed = true;
    g_reason = reason;
}

bool LastRunLost(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::vector<char> chunk(1 << 20);
    // The end of each chunk stays for a line that spans two.
    std::string window;
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = file.gcount();
        if (got <= 0) {
            break;
        }
        window.append(chunk.data(), static_cast<size_t>(got));
        if (window.find(LossLine) != std::string::npos) {
            return true;
        }
        window.erase(0, window.size() - std::min(window.size(), LossLine.size() - 1));
    }
    return false;
}

void Start(const Device& device, Memory& memory, u32 frames_in_flight) {
    g_memory = &memory;
    g_frames_in_flight = frames_in_flight;
    g_check_frames = std::getenv("GR2PORT_CHECK_ADDRESSES") != nullptr;
    if (!g_armed) {
        return;
    }
    g_crumbs = memory.CreateBuffer(MaxMarks * sizeof(u32), vk::BufferUsageFlagBits::eTransferDst,
                                   Placement::Readback);
    if (g_crumbs.mapped == nullptr) {
        g_armed = false;
        return;
    }
    std::memset(g_crumbs.mapped, 0, g_crumbs.size);
    LOG_INFO(Render_Vulkan,
             "GR2port: the GPU trace is on ({}): the GPU writes each pass it begins, for the log "
             "of a lost device",
             g_reason);
}

void AddBuffer(u64 address, u64 size, const std::source_location& where) {
    if (address == 0) {
        return;
    }
    const std::scoped_lock lock{g_buffers_mutex};
    g_buffers[address] = {
        .address = address, .size = size, .file = where.file_name(), .line = where.line()};
    g_ranges_stale = true;
}

void RemoveBuffer(u64 address) {
    const std::scoped_lock lock{g_buffers_mutex};
    const auto found = g_buffers.find(address);
    if (found == g_buffers.end()) {
        return;
    }
    BufferNote& freed = g_freed[g_freed_count++ % KeptFreed];
    freed = found->second;
    freed.freed = g_frame.load(std::memory_order_relaxed);
    g_buffers.erase(found);
    g_ranges_stale = true;
}

void Mark(const Device& device, vk::CommandBuffer cmd, Pass pass) {
    u64 frame = g_frame.load(std::memory_order_relaxed);
    if (pass == Pass::FrameStart) {
        frame++;
        g_frame.store(frame, std::memory_order_relaxed);
        FrameNote& fresh = g_frames[frame % KeptFrames];
        fresh.frame = frame;
        fresh.marks = 0;
        fresh.packet = nullptr;
    }
    FrameNote& note = g_frames[frame % KeptFrames];
    if (note.marks < MaxMarks) {
        note.passes[note.marks++] = static_cast<u8>(pass);
    }
    if (device.checkpoints) {
        const u64 marker = (frame << PassBits) | static_cast<u64>(pass);
        cmd.setCheckpointNV(reinterpret_cast<const void*>(marker), device.dispatch);
    }
    if (g_armed) {
        cmd.fillBuffer(g_crumbs.buffer, static_cast<u64>(pass) * sizeof(u32), sizeof(u32),
                       static_cast<u32>(frame), device.dispatch);
    }
}

void Mark(const Device& device, vk::CommandBuffer cmd, Pass pass,
          const vk::RenderingInfo& rendering) {
    if (!g_armed) {
        Mark(device, cmd, pass);
        return;
    }
    cmd.endRendering(device.dispatch);
    Mark(device, cmd, pass);
    cmd.beginRendering(rendering, device.dispatch);
}

void Note(const Scene::Header& header, const u8* packet) {
    FrameNote& note = g_frames[g_frame.load(std::memory_order_relaxed) % KeptFrames];
    note.header = header;
    note.packet = packet;
    if (!g_check_frames) {
        return;
    }
    const Checked checked = CheckPacket(note);
    if (!g_logged_check && header.draws != 0) {
        g_logged_check = true;
        LOG_INFO(Render_Vulkan,
                 "GR2port: frame {} (game frame {}): {} device addresses checked, {} bad; every "
                 "frame is checked",
                 note.frame, header.frame, checked.addresses, checked.bad);
    }
}

void Submitted() {
    g_submitted = std::chrono::steady_clock::now();
    g_submitted_frame = g_frame.load(std::memory_order_relaxed);
}

void Check(const Device& device, vk::Result result, std::string_view call) {
    if (result != vk::Result::eErrorDeviceLost || g_reported) {
        return;
    }
    g_reported = true;
    const u64 frame = g_frame.load(std::memory_order_relaxed);
    LOG_ERROR(Render_Vulkan, "{}", LossLine);
    const double since =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_submitted)
            .count();
    LOG_ERROR(Render_Vulkan,
              "GR2port: {} reported it, {:.0f} ms after frame {} went to the GPU; the frame "
              "recorded last is {}",
              call, g_submitted_frame == 0 ? 0.0 : since, g_submitted_frame, frame);
    ReportFrames(frame);
    ReportMemory();
    ReportCheckpoints(device);
    ReportFault(device);
    ReportCrumbs(frame);
    // The packets are read last, from memory the lost device maps.
    Common::Log::Flush();
    ReportPackets(frame);
    Common::Log::Flush();
}

} // namespace Gr2Port::DeviceLost
