// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "graphics/gr2port/catcher_marks.h"
#include "graphics/gr2port/catcher_note.h"
#include "graphics/gr2port/device_lost.h"
#include "graphics/gr2port/memory.h"

namespace Gr2Port::Catcher::Marks {

namespace {

enum class Kind : u8 {
    Draw,
    DrawIndexed,
    DrawIndirect,
    DrawIndexedIndirect,
    Dispatch,
    DispatchIndirect,
};

// A draw or dispatch the render thread recorded: its pipeline, the frame and pass it belongs to and
// its arguments.
struct Call {
    u32 serial = 0;
    u32 frame = 0;
    DeviceLost::Pass pass = DeviceLost::Pass::FrameStart;
    Kind kind = Kind::Draw;
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::array<u64, 4> args{};
};

// The marks' buffer holds Slots reached marks, then Slots finished ones; a call's marks go to the
// slots of its serial, so no write lands where an earlier one is still on its way.
constexpr u32 Slots = 4096;
// The calls kept for the report: several frames in flight and those before them.
constexpr u32 KeptCalls = 65536;
constexpr u32 ShownCalls = 16;

struct Code {
    u64 sum = 0;
    u32 words = 0;
};

PFN_vkCmdBindPipeline g_bind = nullptr;
PFN_vkCmdWriteBufferMarkerAMD g_write_mark = nullptr;
PFN_vkCmdDraw g_draw = nullptr;
PFN_vkCmdDrawIndexed g_draw_indexed = nullptr;
PFN_vkCmdDrawIndirect g_draw_indirect = nullptr;
PFN_vkCmdDrawIndexedIndirect g_draw_indexed_indirect = nullptr;
PFN_vkCmdDispatch g_dispatch = nullptr;
PFN_vkCmdDispatchIndirect g_dispatch_indirect = nullptr;
PFN_vkCreateShaderModule g_create_module = nullptr;
PFN_vkDestroyShaderModule g_destroy_module = nullptr;
PFN_vkCreateGraphicsPipelines g_create_graphics = nullptr;
PFN_vkCreateComputePipelines g_create_compute = nullptr;
PFN_vkDestroyPipeline g_destroy_pipeline = nullptr;

// The shader modules and pipelines by their code.
std::mutex g_mutex;
std::unordered_map<VkShaderModule, Code> g_modules;
std::unordered_map<VkPipeline, std::string> g_pipelines;

std::mutex g_calls_mutex;
std::array<Call, KeptCalls> g_calls{};
u32 g_serial = 0;

Buffer g_marks;
thread_local bool t_marking = false;
// The pipelines bound last on this thread, graphics then compute.
thread_local std::array<VkPipeline, 2> t_bound{};

// FNV-1a over the code, as the pipeline module's reports sum it.
u64 Sum(const u32* code, size_t bytes) {
    u64 sum = 0xcbf29ce484222325;
    for (const std::byte byte : std::as_bytes(std::span{code, bytes / sizeof(u32)})) {
        sum = (sum ^ std::to_integer<u64>(byte)) * 0x100000001b3;
    }
    return sum;
}

const char* StageName(VkShaderStageFlagBits stage) {
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:
        return "vertex";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
        return "tessellation control";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
        return "tessellation evaluation";
    case VK_SHADER_STAGE_GEOMETRY_BIT:
        return "geometry";
    case VK_SHADER_STAGE_FRAGMENT_BIT:
        return "fragment";
    case VK_SHADER_STAGE_COMPUTE_BIT:
        return "compute";
    default:
        return "stage";
    }
}

// g_mutex held: the stages of a pipeline, each with its code's sum and size.
std::string DescribeStages(std::span<const VkPipelineShaderStageCreateInfo> stages) {
    std::string described;
    for (const VkPipelineShaderStageCreateInfo& stage : stages) {
        Code code;
        if (const auto found = g_modules.find(stage.module); found != g_modules.end()) {
            code = found->second;
        } else if (const auto* inline_code =
                       static_cast<const VkShaderModuleCreateInfo*>(stage.pNext);
                   inline_code != nullptr &&
                   inline_code->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            code = {Sum(inline_code->pCode, inline_code->codeSize),
                    static_cast<u32>(inline_code->codeSize / sizeof(u32))};
        }
        described += fmt::format("{}{} {:016x} of {} words", described.empty() ? "" : ", ",
                                 StageName(stage.stage), code.sum, code.words);
    }
    return described;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice device,
                                                  const VkShaderModuleCreateInfo* info,
                                                  const VkAllocationCallbacks* allocator,
                                                  VkShaderModule* module) {
    const VkResult result = g_create_module(device, info, allocator, module);
    if (result == VK_SUCCESS) {
        const Code code{Sum(info->pCode, info->codeSize),
                        static_cast<u32>(info->codeSize / sizeof(u32))};
        const std::scoped_lock lock{g_mutex};
        g_modules[*module] = code;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice device, VkShaderModule module,
                                               const VkAllocationCallbacks* allocator) {
    {
        const std::scoped_lock lock{g_mutex};
        g_modules.erase(module);
    }
    g_destroy_module(device, module, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache,
                                                       u32 count,
                                                       const VkGraphicsPipelineCreateInfo* infos,
                                                       const VkAllocationCallbacks* allocator,
                                                       VkPipeline* pipelines) {
    const VkResult result = g_create_graphics(device, cache, count, infos, allocator, pipelines);
    const std::scoped_lock lock{g_mutex};
    for (u32 i = 0; i < count; i++) {
        if (pipelines[i] != VK_NULL_HANDLE) {
            g_pipelines[pipelines[i]] = DescribeStages({infos[i].pStages, infos[i].stageCount});
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice device, VkPipelineCache cache,
                                                      u32 count,
                                                      const VkComputePipelineCreateInfo* infos,
                                                      const VkAllocationCallbacks* allocator,
                                                      VkPipeline* pipelines) {
    const VkResult result = g_create_compute(device, cache, count, infos, allocator, pipelines);
    const std::scoped_lock lock{g_mutex};
    for (u32 i = 0; i < count; i++) {
        if (pipelines[i] != VK_NULL_HANDLE) {
            g_pipelines[pipelines[i]] = DescribeStages({&infos[i].stage, 1});
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice device, VkPipeline pipeline,
                                           const VkAllocationCallbacks* allocator) {
    {
        const std::scoped_lock lock{g_mutex};
        g_pipelines.erase(pipeline);
    }
    g_destroy_pipeline(device, pipeline, allocator);
}

VKAPI_ATTR void VKAPI_CALL BindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                        VkPipeline pipeline) {
    g_bind(cmd, point, pipeline);
    if (point == VK_PIPELINE_BIND_POINT_GRAPHICS || point == VK_PIPELINE_BIND_POINT_COMPUTE) {
        t_bound[point == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0] = pipeline;
    }
}

// The serial of the call the render thread records, after its reached mark; 0 when the call is
// not marked.
u32 Begin(VkCommandBuffer cmd, Kind kind, std::array<u64, 4> args) {
    if (!t_marking) {
        return 0;
    }
    const bool compute = kind == Kind::Dispatch || kind == Kind::DispatchIndirect;
    u32 serial;
    {
        const std::scoped_lock lock{g_calls_mutex};
        serial = ++g_serial;
        if (serial == 0) {
            serial = ++g_serial;
        }
        g_calls[serial % KeptCalls] = {
            .serial = serial,
            .frame = static_cast<u32>(DeviceLost::Frame()),
            .pass = DeviceLost::Current(),
            .kind = kind,
            .pipeline = t_bound[compute ? 1 : 0],
            .args = args,
        };
    }
    g_write_mark(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_marks.buffer,
                 (serial % Slots) * sizeof(u32), serial);
    return serial;
}

void End(VkCommandBuffer cmd, u32 serial) {
    if (serial != 0) {
        g_write_mark(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_marks.buffer,
                     (Slots + serial % Slots) * sizeof(u32), serial);
    }
}

VKAPI_ATTR void VKAPI_CALL Draw(VkCommandBuffer cmd, u32 vertices, u32 instances, u32 first_vertex,
                                u32 first_instance) {
    const u32 serial = Begin(cmd, Kind::Draw, {vertices, instances, first_vertex, first_instance});
    g_draw(cmd, vertices, instances, first_vertex, first_instance);
    End(cmd, serial);
}

VKAPI_ATTR void VKAPI_CALL DrawIndexed(VkCommandBuffer cmd, u32 indices, u32 instances,
                                       u32 first_index, s32 vertex_offset, u32 first_instance) {
    const u32 serial = Begin(cmd, Kind::DrawIndexed,
                             {indices, instances, first_index, static_cast<u64>(vertex_offset)});
    g_draw_indexed(cmd, indices, instances, first_index, vertex_offset, first_instance);
    End(cmd, serial);
}

VKAPI_ATTR void VKAPI_CALL DrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                        u32 count, u32 stride) {
    const u32 serial =
        Begin(cmd, Kind::DrawIndirect, {reinterpret_cast<u64>(buffer), offset, count, stride});
    g_draw_indirect(cmd, buffer, offset, count, stride);
    End(cmd, serial);
}

VKAPI_ATTR void VKAPI_CALL DrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer,
                                               VkDeviceSize offset, u32 count, u32 stride) {
    const u32 serial = Begin(cmd, Kind::DrawIndexedIndirect,
                             {reinterpret_cast<u64>(buffer), offset, count, stride});
    g_draw_indexed_indirect(cmd, buffer, offset, count, stride);
    End(cmd, serial);
}

VKAPI_ATTR void VKAPI_CALL Dispatch(VkCommandBuffer cmd, u32 x, u32 y, u32 z) {
    const u32 serial = Begin(cmd, Kind::Dispatch, {x, y, z, 0});
    g_dispatch(cmd, x, y, z);
    End(cmd, serial);
}

VKAPI_ATTR void VKAPI_CALL DispatchIndirect(VkCommandBuffer cmd, VkBuffer buffer,
                                            VkDeviceSize offset) {
    const u32 serial =
        Begin(cmd, Kind::DispatchIndirect, {reinterpret_cast<u64>(buffer), offset, 0, 0});
    g_dispatch_indirect(cmd, buffer, offset);
    End(cmd, serial);
}

// g_calls_mutex held: the serial among slots fewest calls before the newest recorded, 0 for none.
u32 Latest(u32 first_slot) {
    const auto* slots = reinterpret_cast<const volatile u32*>(g_marks.mapped) + first_slot;
    u32 latest = 0;
    u32 fewest = KeptCalls;
    for (u32 i = 0; i < Slots; i++) {
        const u32 serial = slots[i];
        const u32 back = g_serial - serial;
        if (serial != 0 && back < fewest) {
            fewest = back;
            latest = serial;
        }
    }
    return latest;
}

// g_calls_mutex held: a kept call by its serial.
std::string Describe(u32 serial) {
    const Call& call = g_calls[serial % KeptCalls];
    if (serial == 0 || call.serial != serial) {
        return fmt::format("call {}, no longer kept", serial);
    }
    std::string what;
    const u64 a = call.args[0];
    const u64 b = call.args[1];
    const u64 c = call.args[2];
    switch (call.kind) {
    case Kind::Draw:
        what = fmt::format("a draw of {} vertices, {} instances", a, b);
        break;
    case Kind::DrawIndexed:
        what = fmt::format("an indexed draw of {} indices from {}, {} instances", a, c, b);
        break;
    case Kind::DrawIndirect:
        what = fmt::format("{} indirect draws, the first at {:#x} of buffer {:#x}", c, b, a);
        break;
    case Kind::DrawIndexedIndirect:
        what =
            fmt::format("{} indexed indirect draws, the first at {:#x} of buffer {:#x}", c, b, a);
        break;
    case Kind::Dispatch:
        what = fmt::format("a dispatch of {}x{}x{} groups", a, b, c);
        break;
    case Kind::DispatchIndirect:
        what = fmt::format("an indirect dispatch from {:#x} of buffer {:#x}", b, a);
        break;
    }
    std::string pipeline = "no pipeline";
    {
        const std::scoped_lock lock{g_mutex};
        if (const auto found = g_pipelines.find(call.pipeline); found != g_pipelines.end()) {
            pipeline = found->second;
        }
    }
    return fmt::format("call {} of frame {} in the pass {}: {}; {}", serial, call.frame,
                       DeviceLost::Name(call.pass), what, pipeline);
}

} // namespace

void Install(Device& device) {
    auto& dl = device.dispatch;
    g_bind = std::exchange(dl.vkCmdBindPipeline, BindPipeline);
    g_create_module = std::exchange(dl.vkCreateShaderModule, CreateShaderModule);
    g_destroy_module = std::exchange(dl.vkDestroyShaderModule, DestroyShaderModule);
    g_create_graphics = std::exchange(dl.vkCreateGraphicsPipelines, CreateGraphicsPipelines);
    g_create_compute = std::exchange(dl.vkCreateComputePipelines, CreateComputePipelines);
    g_destroy_pipeline = std::exchange(dl.vkDestroyPipeline, DestroyPipeline);
    if (!device.buffer_marker || dl.vkCmdWriteBufferMarkerAMD == nullptr) {
        return;
    }
    g_write_mark = dl.vkCmdWriteBufferMarkerAMD;
    g_draw = std::exchange(dl.vkCmdDraw, Draw);
    g_draw_indexed = std::exchange(dl.vkCmdDrawIndexed, DrawIndexed);
    g_draw_indirect = std::exchange(dl.vkCmdDrawIndirect, DrawIndirect);
    g_draw_indexed_indirect = std::exchange(dl.vkCmdDrawIndexedIndirect, DrawIndexedIndirect);
    g_dispatch = std::exchange(dl.vkCmdDispatch, Dispatch);
    g_dispatch_indirect = std::exchange(dl.vkCmdDispatchIndirect, DispatchIndirect);
}

void Start(Memory& memory) {
    if (g_write_mark != nullptr) {
        g_marks = memory.CreateBuffer(2 * Slots * sizeof(u32),
                                      vk::BufferUsageFlagBits::eTransferDst, Placement::Readback);
        if (g_marks.mapped != nullptr) {
            std::memset(g_marks.mapped, 0, g_marks.size);
            t_marking = true;
        }
    }
    LOG_INFO(Render_Vulkan, "GR2port catcher: {}",
             t_marking ? "the GPU marks each draw and dispatch as it reaches and finishes it"
             : g_write_mark != nullptr ? "no draw marks: their buffer failed"
                                       : "no draw marks: the device lacks VK_AMD_buffer_marker");
}

std::string DescribeProgress() {
    if (g_marks.mapped == nullptr) {
        return "no draw marks";
    }
    const std::scoped_lock lock{g_calls_mutex};
    const u32 finished = Latest(Slots);
    const u32 reached = Latest(0);
    return fmt::format("draw marks: of {} calls recorded the GPU finished {}; it reached {}",
                       g_serial, finished != 0 ? Describe(finished) : "none",
                       reached != 0 ? Describe(reached) : "none");
}

void Report() {
    if (g_marks.mapped == nullptr) {
        LOG_ERROR(Render_Vulkan, "GR2port catcher: no draw marks in this run");
        return;
    }
    const std::scoped_lock lock{g_calls_mutex};
    const u32 finished = Latest(Slots);
    const u32 reached = Latest(0);
    LOG_ERROR(Render_Vulkan,
              "GR2port catcher: draw marks: of {} calls recorded the GPU finished call {} and "
              "reached call {}",
              g_serial, finished, reached);
    if (finished != 0) {
        LOG_ERROR(Render_Vulkan, "GR2port catcher: finished last: {}", Describe(finished));
    }
    // The calls after the last one finished: the GPU began those up to the one it reached.
    const u32 shown = std::min(g_serial - finished, ShownCalls);
    for (u32 i = 1; i <= shown; i++) {
        const u32 serial = finished + i;
        const bool begun = reached != 0 && reached - serial < KeptCalls;
        LOG_ERROR(Render_Vulkan, "GR2port catcher: {}: {}",
                  begun ? "begun, not finished" : "not begun", Describe(serial));
    }
}

} // namespace Gr2Port::Catcher::Marks
