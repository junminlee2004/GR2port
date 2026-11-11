// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "graphics/gr2port/emulated_gpu.h"
#include "graphics/gr2port/imgui_relay.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/overlays.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/scene.h"
#include "graphics/gr2port/shaders/imgui_relay_frag.h"
#include "graphics/gr2port/shaders/imgui_relay_vert.h"
#include "graphics/gr2port/textures.h"
#include "graphics/gr2port/window.h"

namespace Gr2Port::ImGuiRelay {

namespace {

static_assert(sizeof(ImDrawIdx) == sizeof(u32));
static_assert(sizeof(ImDrawVert) == 5 * sizeof(u32));

// Textures go by address, ImGui's own (the font atlas) with the top bit set.
constexpr u64 OwnTexture = u64{1} << 63;
constexpr u32 PixelBytes = 4;
// The emulator's window the game's picture fills; GR2port draws its own picture there.
constexpr const char* GameDisplay = "Display##game_display";

// A texture's pixels as the emulator last wrote them.
struct Pixels {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> bytes;
    // Counts the times the texture is made; the renderer uploads a new one whole.
    u32 generation = 0;
    // The region written since the renderer's last upload: left, top, right, bottom.
    bool dirty = false;
    std::array<u32, 4> region{};
};

struct Command {
    u32 texture;
    u32 index_count;
    u32 first_index;
    u32 vertex_offset;
    ImVec4 clip;
};

// One ImGui frame: its draw lists one after another, in the emulator window's coordinates.
struct Snapshot {
    ImVec2 position;
    ImVec2 size;
    std::vector<ImDrawVert> vertices;
    std::vector<ImDrawIdx> indices;
    std::vector<Command> commands;
};

std::mutex g_mutex;
// Under g_mutex: the textures by key, their pixels, the textures gone since the renderer's last
// update, and the latest frame.
std::unordered_map<u64, u32> g_textures;
std::unordered_map<u32, Pixels> g_pixels;
std::vector<u32> g_gone;
u32 g_next_texture = 1;
Snapshot g_latest;
bool g_fresh = false;
// Present thread.
Snapshot g_capturing;

// For the main thread's events: whether the windows show on GR2port's, and where. A pixel of
// GR2port's window is left + x * scale for x in the game window's coordinates.
std::atomic<bool> g_active{false};
std::atomic<float> g_view_left{0.0f};
std::atomic<float> g_view_top{0.0f};
std::atomic<float> g_view_scale{0.0f};
// The mouse's last position over GR2port's window, in its coordinates, while it is there.
std::atomic<bool> g_mouse_inside{false};
std::atomic<float> g_mouse_x{0.0f};
std::atomic<float> g_mouse_y{0.0f};

// Render thread.
struct Resident {
    Image image;
    u32 handle = 0;
    u32 generation = 0;
    bool ready = false;
};

// Freed once the frame is done.
struct Retired {
    u64 frame;
    Image image;
    u32 handle;
    Buffer buffer;
};

// A frame's vertices, then its indices.
struct Slot {
    Buffer buffer;
    u64 frame = 0;
    u64 indices = 0;
};

// More than the renderer's frames in flight, so the frame's slot is free.
constexpr u32 Slots = 3;
constexpr u64 SlotBytes = 1_MB;
constexpr vk::BufferUsageFlags SlotUsage =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndexBuffer;

struct Push {
    vk::DeviceAddress vertices;
    std::array<float, 2> scale;
    std::array<float, 2> translate;
    u32 texture;
    u32 pad;
};
static_assert(sizeof(Push) == 32);

// As the emulator's backend: colour over the target by alpha, and alpha accumulated.
constexpr vk::PipelineColorBlendAttachmentState Blend{
    .blendEnable = true,
    .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
    .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
    .colorBlendOp = vk::BlendOp::eAdd,
    .srcAlphaBlendFactor = vk::BlendFactor::eOne,
    .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
    .alphaBlendOp = vk::BlendOp::eAdd,
    .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                      vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
Pipeline g_pipeline;
std::unordered_map<u32, Resident> g_resident;
std::vector<u32> g_free_handles;
std::vector<Retired> g_retired;
std::vector<u32> g_retiring;
std::array<Slot, Slots> g_slots;
Snapshot g_drawn;
// The slot of the frame's vertices, when it has commands.
Slot* g_slot = nullptr;
bool g_logged_drawing = false;
bool g_logged_full = false;

u64 KeyOf(const ImGui::Texture* texture) {
    return reinterpret_cast<std::uintptr_t>(texture);
}

u64 KeyOf(const ImTextureData& texture) {
    return OwnTexture | reinterpret_cast<std::uintptr_t>(&texture);
}

// Under g_mutex.
void ForgetKey(u64 key) {
    const auto it = g_textures.find(key);
    if (it == g_textures.end()) {
        return;
    }
    g_pixels.erase(it->second);
    g_gone.push_back(it->second);
    g_textures.erase(it);
}

// Under g_mutex: the pixels of a texture made, or made again.
Pixels& Make(u64 key, u32 width, u32 height) {
    const auto [it, inserted] = g_textures.try_emplace(key, g_next_texture);
    if (inserted) {
        g_next_texture++;
    }
    Pixels& pixels = g_pixels[it->second];
    pixels.width = width;
    pixels.height = height;
    pixels.generation++;
    pixels.dirty = false;
    return pixels;
}

// Under g_mutex: copies what ImGui asks of the backend for its own textures.
void TakeOwnTextures(const ImDrawData& draw_data) {
    if (draw_data.Textures == nullptr) {
        return;
    }
    for (const ImTextureData* texture : *draw_data.Textures) {
        const u64 key = KeyOf(*texture);
        if (texture->Status == ImTextureStatus_WantDestroy) {
            ForgetKey(key);
            continue;
        }
        if (texture->Format != ImTextureFormat_RGBA32 || texture->Pixels == nullptr) {
            continue;
        }
        if (texture->Status == ImTextureStatus_WantCreate) {
            Pixels& pixels =
                Make(key, static_cast<u32>(texture->Width), static_cast<u32>(texture->Height));
            pixels.bytes.assign(texture->Pixels, texture->Pixels + texture->GetSizeInBytes());
            continue;
        }
        const auto it = g_textures.find(key);
        if (texture->Status != ImTextureStatus_WantUpdates || it == g_textures.end()) {
            continue;
        }
        Pixels& pixels = g_pixels[it->second];
        const u32 left = texture->UpdateRect.x;
        const u32 top = texture->UpdateRect.y;
        const u32 right = left + texture->UpdateRect.w;
        const u32 bottom = top + texture->UpdateRect.h;
        if (right <= left || bottom <= top || right > pixels.width || bottom > pixels.height) {
            continue;
        }
        for (u32 row = top; row < bottom; row++) {
            const size_t offset = (size_t{row} * pixels.width + left) * PixelBytes;
            std::memcpy(pixels.bytes.data() + offset, texture->Pixels + offset,
                        size_t{right - left} * PixelBytes);
        }
        if (!pixels.dirty) {
            pixels.region = {left, top, right, bottom};
            pixels.dirty = true;
        } else {
            pixels.region = {std::min(pixels.region[0], left), std::min(pixels.region[1], top),
                             std::max(pixels.region[2], right), std::max(pixels.region[3], bottom)};
        }
    }
}

// Under g_mutex: the texture a draw samples, or 0 for one GR2port does not have.
u32 TextureOf(const ImTextureRef& ref) {
    const u64 key = ref._TexData != nullptr ? KeyOf(*ref._TexData) : KeyOf(ref._TexID);
    const auto it = g_textures.find(key);
    return it != g_textures.end() ? it->second : 0;
}

// Under g_mutex: brings the renderer's copy of a texture up to its pixels.
void Upload(vk::CommandBuffer cmd, u64 frame, u32 texture, Pixels& pixels) {
    const auto& dl = g_device->dispatch;
    Resident& resident = g_resident[texture];
    const bool whole = !resident.image.image || resident.generation != pixels.generation;
    if (!whole && !pixels.dirty) {
        return;
    }
    if (whole) {
        if (resident.image.image) {
            g_retired.push_back(
                {.frame = frame, .image = resident.image, .handle = resident.handle});
            resident = {};
        }
        Image image;
        if (!g_free_handles.empty()) {
            image = g_memory->CreateTexture(vk::Format::eR8G8B8A8Unorm,
                                            {pixels.width, pixels.height}, 1, {});
        }
        if (!image.image) {
            if (!g_logged_full) {
                LOG_WARNING(Render_Vulkan,
                            "GR2port: an ImGui texture of {}x{} has no handle or no memory",
                            pixels.width, pixels.height);
                g_logged_full = true;
            }
            g_resident.erase(texture);
            return;
        }
        resident.image = image;
        resident.handle = g_free_handles.back();
        g_free_handles.pop_back();
        resident.generation = pixels.generation;
        Textures::SetExternal(resident.handle, image.view);
    }
    const std::array<u32, 4> region =
        whole ? std::array<u32, 4>{0, 0, pixels.width, pixels.height} : pixels.region;
    const u32 width = region[2] - region[0];
    const u32 height = region[3] - region[1];
    Buffer staging =
        g_memory->CreateBuffer(u64{width} * height * PixelBytes,
                               vk::BufferUsageFlagBits::eTransferSrc, Placement::Staging);
    if (staging.mapped == nullptr) {
        g_memory->Destroy(staging);
        // A new image stays unsampled, and is made again next frame.
        if (whole) {
            resident.generation = 0;
        }
        return;
    }
    for (u32 row = 0; row < height; row++) {
        const size_t source = (size_t{region[1] + row} * pixels.width + region[0]) * PixelBytes;
        std::memcpy(staging.mapped + size_t{row} * width * PixelBytes, pixels.bytes.data() + source,
                    size_t{width} * PixelBytes);
    }
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask =
            whole ? vk::PipelineStageFlagBits2::eNone : vk::PipelineStageFlagBits2::eFragmentShader,
        .srcAccessMask = vk::AccessFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = whole ? vk::ImageLayout::eUndefined : vk::ImageLayout::eShaderReadOnlyOptimal,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = resident.image.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dl);
    const vk::BufferImageCopy copy{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {static_cast<s32>(region[0]), static_cast<s32>(region[1]), 0},
        .imageExtent = {width, height, 1},
    };
    cmd.copyBufferToImage(staging.buffer, resident.image.image,
                          vk::ImageLayout::eTransferDstOptimal, copy, dl);
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier}, dl);
    g_retired.push_back({.frame = frame, .handle = 0, .buffer = staging});
    resident.ready = true;
    pixels.dirty = false;
}

} // namespace

void Capture(const ImDrawData& draw_data) {
    // GR2fork's window shows the windows itself, unless an overlay keeps it from presenting.
    const bool wanted = !EmulatedGpu::On() || OverlayHooksEveryDevice();
    g_active.store(wanted, std::memory_order_relaxed);
    Snapshot& snapshot = g_capturing;
    snapshot.position = draw_data.DisplayPos;
    snapshot.size = draw_data.DisplaySize;
    snapshot.vertices.clear();
    snapshot.indices.clear();
    snapshot.commands.clear();
    // The game's display window, and the dock host that draws its background.
    const ImDrawList* display = nullptr;
    const ImDrawList* host = nullptr;
    if (const ImGuiWindow* window = ImGui::FindWindowByName(GameDisplay); window != nullptr) {
        display = window->DrawList;
        if (window->DockNode != nullptr && window->DockNode->HostWindow != nullptr) {
            host = window->DockNode->HostWindow->DrawList;
        }
    }
    std::scoped_lock lock{g_mutex};
    TakeOwnTextures(draw_data);
    for (const ImDrawList* list : draw_data.CmdLists) {
        if (!wanted || list == display || list == host) {
            continue;
        }
        const u32 vertex_base = static_cast<u32>(snapshot.vertices.size());
        const u32 index_base = static_cast<u32>(snapshot.indices.size());
        snapshot.vertices.insert(snapshot.vertices.end(), list->VtxBuffer.begin(),
                                 list->VtxBuffer.end());
        snapshot.indices.insert(snapshot.indices.end(), list->IdxBuffer.begin(),
                                list->IdxBuffer.end());
        for (const ImDrawCmd& draw : list->CmdBuffer) {
            const u32 texture = TextureOf(draw.TexRef);
            if (draw.UserCallback != nullptr || draw.ElemCount == 0 || texture == 0) {
                continue;
            }
            snapshot.commands.push_back({
                .texture = texture,
                .index_count = draw.ElemCount,
                .first_index = index_base + draw.IdxOffset,
                .vertex_offset = vertex_base + draw.VtxOffset,
                .clip = draw.ClipRect,
            });
        }
    }
    std::swap(g_capturing, g_latest);
    g_fresh = true;
}

void KeepPixels(const ImGui::Texture* texture, const void* pixels, u32 width, u32 height) {
    if (texture == nullptr || pixels == nullptr || width == 0 || height == 0) {
        return;
    }
    const auto* const bytes = static_cast<const u8*>(pixels);
    std::vector<u8> copy(bytes, bytes + size_t{width} * height * PixelBytes);
    std::scoped_lock lock{g_mutex};
    Make(KeyOf(texture), width, height).bytes = std::move(copy);
}

void Forget(const ImGui::Texture* texture) {
    std::scoped_lock lock{g_mutex};
    ForgetKey(KeyOf(texture));
}

const SDL_Event* ForImGui(const SDL_Event* event, SDL_Event& copy) {
    const u32 own = Window::Id();
    const u32 game = Window::GameId();
    SDL_Window* const window = own != 0 && game != 0 ? SDL_GetWindowFromEvent(event) : nullptr;
    if (window == nullptr) {
        return event;
    }
    // Set again below while the mouse moves over GR2port's window.
    if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        g_mouse_inside.store(false, std::memory_order_relaxed);
    }
    if (SDL_GetWindowID(window) != own || !g_active.load(std::memory_order_relaxed)) {
        return event;
    }
    copy = *event;
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        copy.key.windowID = game;
        return &copy;
    case SDL_EVENT_TEXT_INPUT:
        copy.text.windowID = game;
        return &copy;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        copy.window.windowID = game;
        return &copy;
    default:
        break;
    }
    const float scale = g_view_scale.load(std::memory_order_relaxed);
    if (scale <= 0.0f) {
        return event;
    }
    // Window coordinates to GR2port's pixels, then to the game window's coordinates.
    float density = SDL_GetWindowPixelDensity(window);
    if (density <= 0.0f) {
        density = 1.0f;
    }
    const float left = g_view_left.load(std::memory_order_relaxed);
    const float top = g_view_top.load(std::memory_order_relaxed);
    const auto to_game = [&](float& x, float& y) {
        x = (x * density - left) / scale;
        y = (y * density - top) / scale;
    };
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        g_mouse_x.store(event->motion.x, std::memory_order_relaxed);
        g_mouse_y.store(event->motion.y, std::memory_order_relaxed);
        g_mouse_inside.store(true, std::memory_order_relaxed);
        copy.motion.windowID = game;
        to_game(copy.motion.x, copy.motion.y);
        return &copy;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        copy.button.windowID = game;
        to_game(copy.button.x, copy.button.y);
        return &copy;
    case SDL_EVENT_MOUSE_WHEEL:
        copy.wheel.windowID = game;
        to_game(copy.wheel.mouse_x, copy.wheel.mouse_y);
        return &copy;
    default:
        return event;
    }
}

bool WarpBack() {
    if (!g_mouse_inside.load(std::memory_order_relaxed)) {
        return false;
    }
    SDL_Window* const window = SDL_GetWindowFromID(Window::Id());
    if (window == nullptr) {
        return false;
    }
    // The same position comes back as the same game window position, so the cursor stays idle.
    SDL_WarpMouseInWindow(window, g_mouse_x.load(std::memory_order_relaxed),
                          g_mouse_y.load(std::memory_order_relaxed));
    return true;
}

bool Start(const Device& device, Memory& memory) {
    g_device = &device;
    g_memory = &memory;
    for (u32 i = Scene::MaxImGuiTextures; i > 0; i--) {
        g_free_handles.push_back(Scene::ImGuiHandles + i - 1);
    }
    for (Slot& slot : g_slots) {
        slot.buffer = memory.CreateBuffer(SlotBytes, SlotUsage, Placement::Upload);
        if (slot.buffer.mapped == nullptr) {
            return false;
        }
    }
    return true;
}

bool Resize(vk::Format window_format) {
    DestroyPipeline(*g_device, g_pipeline);
    const std::array sets{Textures::SetLayout()};
    g_pipeline = CreateGraphicsPipeline(*g_device, {
                                                       .vertex = IMGUI_RELAY_VERT,
                                                       .fragment = IMGUI_RELAY_FRAG,
                                                       .color = window_format,
                                                       .blend = &Blend,
                                                       .push_constant_bytes = sizeof(Push),
                                                       .set_layouts = sets,
                                                   });
    return static_cast<bool>(g_pipeline.pipeline);
}

void Update(vk::CommandBuffer cmd, u64 frame, u64 completed) {
    std::erase_if(g_retired, [completed](Retired& retired) {
        if (retired.frame > completed) {
            return false;
        }
        g_memory->Destroy(retired.image);
        g_memory->Destroy(retired.buffer);
        if (retired.handle != 0) {
            g_free_handles.push_back(retired.handle);
        }
        return true;
    });
    g_slot = nullptr;
    {
        std::scoped_lock lock{g_mutex};
        if (g_fresh) {
            std::swap(g_latest, g_drawn);
            g_fresh = false;
        }
        g_retiring.swap(g_gone);
        for (const Command& command : g_drawn.commands) {
            const auto pixels = g_pixels.find(command.texture);
            if (pixels != g_pixels.end()) {
                Upload(cmd, frame, command.texture, pixels->second);
            }
        }
    }
    for (const u32 texture : g_retiring) {
        const auto it = g_resident.find(texture);
        if (it != g_resident.end()) {
            g_retired.push_back(
                {.frame = frame, .image = it->second.image, .handle = it->second.handle});
            g_resident.erase(it);
        }
    }
    g_retiring.clear();
    Slot& slot = g_slots[frame % Slots];
    if (g_drawn.commands.empty() || !g_pipeline.pipeline || slot.frame > completed) {
        return;
    }
    const u64 vertex_bytes = g_drawn.vertices.size() * sizeof(ImDrawVert);
    const u64 indices = Common::AlignUp(vertex_bytes, 16);
    const u64 bytes = indices + g_drawn.indices.size() * sizeof(ImDrawIdx);
    if (slot.buffer.size < bytes) {
        g_memory->Destroy(slot.buffer);
        slot.buffer = g_memory->CreateBuffer(std::bit_ceil(bytes), SlotUsage, Placement::Upload);
        if (slot.buffer.mapped == nullptr) {
            g_memory->Destroy(slot.buffer);
            return;
        }
    }
    std::memcpy(slot.buffer.mapped, g_drawn.vertices.data(), vertex_bytes);
    std::memcpy(slot.buffer.mapped + indices, g_drawn.indices.data(),
                g_drawn.indices.size() * sizeof(ImDrawIdx));
    slot.frame = frame;
    slot.indices = indices;
    g_slot = &slot;
    if (!g_logged_drawing) {
        LOG_INFO(Render_Vulkan, "GR2port: drawing the emulator's ImGui windows");
        g_logged_drawing = true;
    }
}

void Record(vk::CommandBuffer cmd, vk::Extent2D extent) {
    const Snapshot& drawn = g_drawn;
    if (drawn.size.x <= 0.0f || drawn.size.y <= 0.0f || extent.width == 0 || extent.height == 0) {
        return;
    }
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    // The emulator's window scaled to fit the renderer's, centred: a pixel is left + x * scale.
    const float scale = std::min(width / drawn.size.x, height / drawn.size.y);
    const float left = (width - drawn.size.x * scale) * 0.5f - drawn.position.x * scale;
    const float top = (height - drawn.size.y * scale) * 0.5f - drawn.position.y * scale;
    g_view_left.store(left, std::memory_order_relaxed);
    g_view_top.store(top, std::memory_order_relaxed);
    g_view_scale.store(scale, std::memory_order_relaxed);
    if (g_slot == nullptr) {
        return;
    }
    const auto& dl = g_device->dispatch;
    Push push{
        .vertices = g_slot->buffer.address,
        .scale = {2.0f * scale / width, 2.0f * scale / height},
        .translate = {2.0f * left / width - 1.0f, 2.0f * top / height - 1.0f},
    };
    const vk::DescriptorSet set = Textures::Set();
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, g_pipeline.pipeline, dl);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, g_pipeline.layout, 0, set, {}, dl);
    cmd.bindIndexBuffer(g_slot->buffer.buffer, g_slot->indices, vk::IndexType::eUint32, dl);
    cmd.setViewport(0, vk::Viewport{.width = width, .height = height, .maxDepth = 1.0f}, dl);
    for (const Command& command : drawn.commands) {
        const auto resident = g_resident.find(command.texture);
        if (resident == g_resident.end() || !resident->second.ready) {
            continue;
        }
        const float x0 = std::clamp(std::floor(left + command.clip.x * scale), 0.0f, width);
        const float y0 = std::clamp(std::floor(top + command.clip.y * scale), 0.0f, height);
        const float x1 = std::clamp(std::ceil(left + command.clip.z * scale), 0.0f, width);
        const float y1 = std::clamp(std::ceil(top + command.clip.w * scale), 0.0f, height);
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        const vk::Rect2D scissor{{static_cast<s32>(x0), static_cast<s32>(y0)},
                                 {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}};
        push.texture = resident->second.handle;
        cmd.pushConstants(g_pipeline.layout, vk::ShaderStageFlagBits::eAllGraphics, 0, sizeof(push),
                          &push, dl);
        cmd.setScissor(0, scissor, dl);
        cmd.drawIndexed(command.index_count, 1, command.first_index,
                        static_cast<s32>(command.vertex_offset), 0, dl);
    }
}

} // namespace Gr2Port::ImGuiRelay
