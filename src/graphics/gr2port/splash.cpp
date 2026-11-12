// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stb_image.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/libraries/font/font_internal.h"
#include "graphics/gr2port/memory.h"
#include "graphics/gr2port/pipeline.h"
#include "graphics/gr2port/splash.h"
#include "graphics/gr2port/swapchain.h"
#include "graphics/gr2port/window.h"

namespace Gr2Port::Splash {

namespace {

constexpr vk::Format PictureFormat = vk::Format::eR8G8B8A8Unorm;
constexpr vk::ImageSubresourceLayers Layers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};

// The thread presents this often; the text's dots move every second time.
constexpr std::chrono::milliseconds Period{250};
// The text's height is this part of the window's, within these pixels.
constexpr u32 LinesPerWindow = 40;
constexpr u32 SmallestText = 14;
constexpr u32 LargestText = 56;
// The label holds the longest text at the largest size.
constexpr vk::Extent2D LabelCapacity{1280, 128};
constexpr std::string_view Font = "@builtin/NotoSans-Regular.ttf";
constexpr std::string_view LongestText = "Compiling shaders 100% ...";
constexpr std::array<std::string_view, 4> Dots{"", " .", " ..", " ..."};
// A dark rim this opaque surrounds the letters, so they read on a bright picture too.
constexpr float RimOpacity = 0.7f;

// A character's coverage, a byte per pixel, and its place on the line.
struct Glyph {
    std::vector<u8> coverage;
    s32 width = 0;
    s32 height = 0;
    s32 left = 0;
    s32 top = 0;
    s32 advance = 0;
};

const Device* g_device = nullptr;
Memory* g_memory = nullptr;
Swapchain* g_swapchain = nullptr;
vk::CommandPool g_pool;
vk::CommandBuffer g_commands;
vk::Fence g_done;
vk::Semaphore g_acquired;
bool g_shown = false;

// The picture on the GPU and its pixels for the CPU; no picture leaves both empty.
Image g_picture;
vk::Extent2D g_picture_extent;
std::vector<u8> g_pixels;

// The label: the text over the part of the picture under it, composed on the CPU.
Image g_label;
Buffer g_label_staging;
bool g_label_written = false;
vk::Rect2D g_box;
std::vector<u8> g_under;
std::vector<u8> g_coverage;
std::vector<u8> g_rim;
std::string g_text;
bool g_text_changed = false;

FT_Library g_library = nullptr;
FT_Face g_face = nullptr;
std::shared_ptr<std::vector<unsigned char>> g_font;
std::array<Glyph, 128> g_glyphs;
s32 g_ascent = 0;
s32 g_pad = 0;
s32 g_rim_radius = 0;

std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_wake;
bool g_stop = false;

void Barrier(vk::CommandBuffer cmd, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             vk::AccessFlags2 src_access, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .srcAccessMask = src_access,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllTransfer,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier},
                         g_device->dispatch);
}

// The picture at the largest size that fits the window, centred.
vk::Rect2D Fit(vk::Extent2D picture, vk::Extent2D window) {
    const float scale = std::min(static_cast<float>(window.width) / picture.width,
                                 static_cast<float>(window.height) / picture.height);
    const u32 width = static_cast<u32>(picture.width * scale);
    const u32 height = static_cast<u32>(picture.height * scale);
    return {{(static_cast<s32>(window.width) - static_cast<s32>(width)) / 2,
             (static_cast<s32>(window.height) - static_cast<s32>(height)) / 2},
            {width, height}};
}

std::array<vk::Offset3D, 2> Corners(vk::Rect2D rect) {
    return {{{rect.offset.x, rect.offset.y, 0},
             {rect.offset.x + static_cast<s32>(rect.extent.width),
              rect.offset.y + static_cast<s32>(rect.extent.height), 1}}};
}

// The picture's colour at a window pixel, as the linear blit to the window gives it, or black
// beside the picture.
std::array<u8, 4> Under(vk::Rect2D fit, s32 x, s32 y) {
    if (g_pixels.empty() || x < fit.offset.x || y < fit.offset.y ||
        x >= fit.offset.x + static_cast<s32>(fit.extent.width) ||
        y >= fit.offset.y + static_cast<s32>(fit.extent.height)) {
        return {0, 0, 0, 255};
    }
    const float u =
        (static_cast<float>(x - fit.offset.x) + 0.5f) * g_picture_extent.width / fit.extent.width -
        0.5f;
    const float v = (static_cast<float>(y - fit.offset.y) + 0.5f) * g_picture_extent.height /
                        fit.extent.height -
                    0.5f;
    const s32 right = static_cast<s32>(g_picture_extent.width) - 1;
    const s32 bottom = static_cast<s32>(g_picture_extent.height) - 1;
    const s32 x0 = std::clamp(static_cast<s32>(std::floor(u)), 0, right);
    const s32 y0 = std::clamp(static_cast<s32>(std::floor(v)), 0, bottom);
    const s32 x1 = std::min(x0 + 1, right);
    const s32 y1 = std::min(y0 + 1, bottom);
    const float fx = std::clamp(u - static_cast<float>(x0), 0.0f, 1.0f);
    const float fy = std::clamp(v - static_cast<float>(y0), 0.0f, 1.0f);
    const auto texel = [](s32 tx, s32 ty) {
        return g_pixels.data() + (static_cast<size_t>(ty) * g_picture_extent.width + tx) * 4;
    };
    std::array<u8, 4> color{0, 0, 0, 255};
    for (u32 c = 0; c < 3; c++) {
        const float upper = texel(x0, y0)[c] + (texel(x1, y0)[c] - texel(x0, y0)[c]) * fx;
        const float lower = texel(x0, y1)[c] + (texel(x1, y1)[c] - texel(x0, y1)[c]) * fx;
        color[c] = static_cast<u8>(std::lround(upper + (lower - upper) * fy));
    }
    return color;
}

s32 TextWidth(std::string_view text) {
    s32 width = 0;
    for (const char c : text) {
        width += g_glyphs[static_cast<u8>(c) & 127].advance;
    }
    return width;
}

// Sizes the text and the label for the window, and keeps the picture's part under the label.
void Layout(vk::Extent2D window) {
    g_box = vk::Rect2D{};
    if (g_face == nullptr || !g_label.image || g_label_staging.mapped == nullptr) {
        return;
    }
    const u32 pixels = std::clamp(window.height / LinesPerWindow, SmallestText, LargestText);
    if (FT_Set_Pixel_Sizes(g_face, 0, pixels) != 0) {
        return;
    }
    for (u32 c = 32; c < g_glyphs.size(); c++) {
        Glyph& glyph = g_glyphs[c];
        glyph = {};
        if (FT_Load_Char(g_face, c, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_GlyphSlot slot = g_face->glyph;
        glyph.width = static_cast<s32>(slot->bitmap.width);
        glyph.height = static_cast<s32>(slot->bitmap.rows);
        glyph.left = slot->bitmap_left;
        glyph.top = slot->bitmap_top;
        glyph.advance = static_cast<s32>(slot->advance.x >> 6);
        glyph.coverage.resize(static_cast<size_t>(glyph.width) * glyph.height);
        for (s32 row = 0; row < glyph.height; row++) {
            std::memcpy(glyph.coverage.data() + static_cast<size_t>(row) * glyph.width,
                        slot->bitmap.buffer + static_cast<ptrdiff_t>(row) * slot->bitmap.pitch,
                        glyph.width);
        }
    }
    g_ascent = static_cast<s32>(g_face->size->metrics.ascender >> 6);
    const s32 descent = static_cast<s32>(-g_face->size->metrics.descender >> 6);
    g_rim_radius = std::max<s32>(1, static_cast<s32>(pixels) / 12);
    g_pad = g_rim_radius + 2;
    // A line and a half from the top, clear of a frame counter an overlay draws in the corner.
    const s32 left = std::max<s32>(0, static_cast<s32>(pixels) * 3 / 4 - g_pad);
    const s32 top = std::max<s32>(0, static_cast<s32>(pixels) * 3 / 2 - g_pad);
    const u32 width = std::min<u32>(TextWidth(LongestText) + 2 * g_pad, LabelCapacity.width);
    const u32 height = std::min<u32>(g_ascent + descent + 2 * g_pad, LabelCapacity.height);
    if (static_cast<u32>(left) + width > window.width ||
        static_cast<u32>(top) + height > window.height) {
        return;
    }
    g_box = vk::Rect2D{{left, top}, {width, height}};
    const vk::Rect2D fit = g_pixels.empty() ? vk::Rect2D{} : Fit(g_picture_extent, window);
    g_under.resize(static_cast<size_t>(width) * height * 4);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const auto color = Under(fit, left + static_cast<s32>(x), top + static_cast<s32>(y));
            std::memcpy(g_under.data() + (static_cast<size_t>(y) * width + x) * 4, color.data(), 4);
        }
    }
    g_text.clear();
}

// Writes the label's pixels: the picture under it, the letters' dark rim, the letters in white.
void Compose(std::string_view text) {
    const s32 width = static_cast<s32>(g_box.extent.width);
    const s32 height = static_cast<s32>(g_box.extent.height);
    const size_t count = static_cast<size_t>(width) * height;
    g_coverage.assign(count, 0);
    s32 pen = g_pad;
    for (const char c : text) {
        const Glyph& glyph = g_glyphs[static_cast<u8>(c) & 127];
        for (s32 row = 0; row < glyph.height; row++) {
            const s32 y = g_pad + g_ascent - glyph.top + row;
            if (y < 0 || y >= height) {
                continue;
            }
            for (s32 column = 0; column < glyph.width; column++) {
                const s32 x = pen + glyph.left + column;
                if (x < 0 || x >= width) {
                    continue;
                }
                u8& covered = g_coverage[static_cast<size_t>(y) * width + x];
                covered = std::max(covered,
                                   glyph.coverage[static_cast<size_t>(row) * glyph.width + column]);
            }
        }
        pen += glyph.advance;
    }
    // The rim is the letters grown by its radius: the largest coverage along the row, then along
    // the column.
    std::vector<u8> rows(count, 0);
    for (s32 y = 0; y < height; y++) {
        for (s32 x = 0; x < width; x++) {
            u8 most = 0;
            for (s32 at = std::max(0, x - g_rim_radius);
                 at <= std::min(width - 1, x + g_rim_radius); at++) {
                most = std::max(most, g_coverage[static_cast<size_t>(y) * width + at]);
            }
            rows[static_cast<size_t>(y) * width + x] = most;
        }
    }
    g_rim.assign(count, 0);
    for (s32 y = 0; y < height; y++) {
        for (s32 x = 0; x < width; x++) {
            u8 most = 0;
            for (s32 at = std::max(0, y - g_rim_radius);
                 at <= std::min(height - 1, y + g_rim_radius); at++) {
                most = std::max(most, rows[static_cast<size_t>(at) * width + x]);
            }
            g_rim[static_cast<size_t>(y) * width + x] = most;
        }
    }
    for (size_t i = 0; i < count; i++) {
        const float rim = 1.0f - RimOpacity * static_cast<float>(g_rim[i]) / 255.0f;
        const float letter = static_cast<float>(g_coverage[i]) / 255.0f;
        u8* const out = g_label_staging.mapped + i * 4;
        for (u32 c = 0; c < 3; c++) {
            const float under = static_cast<float>(g_under[i * 4 + c]) * rim;
            out[c] = static_cast<u8>(std::lround(under + (255.0f - under) * letter));
        }
        out[3] = 255;
    }
}

// Submits the commands and waits for them; done is signalled either way.
bool Submit(const vk::SubmitInfo2& submit) {
    const auto& dl = g_device->dispatch;
    const bool submitted = g_device->queue.submit2(submit, g_done, dl) == vk::Result::eSuccess;
    if (!submitted) {
        (void)g_device->queue.submit2(vk::SubmitInfo2{}, g_done, dl);
    }
    (void)g_device->device.waitForFences(g_done, true, UINT64_MAX, dl);
    return submitted;
}

vk::CommandBuffer Begin() {
    const auto& dl = g_device->dispatch;
    (void)g_device->device.resetFences(g_done, dl);
    (void)g_device->device.resetCommandPool(g_pool, {}, dl);
    (void)g_commands.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}, dl);
    return g_commands;
}

// Presents the picture over black, and the label over it when it has text. False when the
// swapchain needs creating.
bool Present(bool with_label) {
    u32 index = 0;
    if (!g_swapchain->Acquire(g_acquired, index)) {
        return false;
    }
    const auto& dl = g_device->dispatch;
    const vk::CommandBuffer cmd = Begin();
    const vk::Image target = g_swapchain->Image(index);
    const vk::Extent2D window = g_swapchain->Extent();
    Barrier(cmd, target, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::AccessFlagBits2::eNone, vk::AccessFlagBits2::eTransferWrite);
    cmd.clearColorImage(target, vk::ImageLayout::eTransferDstOptimal,
                        vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}},
                        vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, dl);
    // Each copy into the window's image comes after the one before it.
    const auto ordered = [&] {
        Barrier(cmd, target, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                vk::AccessFlagBits2::eTransferWrite);
    };
    if (g_picture.image) {
        ordered();
        const vk::ImageBlit blit{
            .srcSubresource = Layers,
            .srcOffsets = Corners({{0, 0}, g_picture_extent}),
            .dstSubresource = Layers,
            .dstOffsets = Corners(Fit(g_picture_extent, window)),
        };
        cmd.blitImage(g_picture.image, vk::ImageLayout::eTransferSrcOptimal, target,
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear, dl);
    }
    if (with_label && g_box.extent.width != 0 && !g_text.empty()) {
        if (g_text_changed) {
            Barrier(cmd, g_label.image,
                    g_label_written ? vk::ImageLayout::eTransferSrcOptimal
                                    : vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eNone,
                    vk::AccessFlagBits2::eTransferWrite);
            const vk::BufferImageCopy copy{
                .imageSubresource = Layers,
                .imageExtent = {g_box.extent.width, g_box.extent.height, 1},
            };
            cmd.copyBufferToImage(g_label_staging.buffer, g_label.image,
                                  vk::ImageLayout::eTransferDstOptimal, copy, dl);
            Barrier(cmd, g_label.image, vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::AccessFlagBits2::eTransferRead);
            g_label_written = true;
            g_text_changed = false;
        }
        ordered();
        const vk::ImageBlit blit{
            .srcSubresource = Layers,
            .srcOffsets = Corners({{0, 0}, g_box.extent}),
            .dstSubresource = Layers,
            .dstOffsets = Corners(g_box),
        };
        cmd.blitImage(g_label.image, vk::ImageLayout::eTransferSrcOptimal, target,
                      vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest, dl);
    }
    Barrier(cmd, target, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
            vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eNone);
    (void)cmd.end(dl);
    const vk::SemaphoreSubmitInfo wait{
        .semaphore = g_acquired,
        .stageMask = vk::PipelineStageFlagBits2::eAllTransfer,
    };
    const vk::SemaphoreSubmitInfo signal{
        .semaphore = g_swapchain->Rendered(index),
        .stageMask = vk::PipelineStageFlagBits2::eAllTransfer,
    };
    const vk::CommandBufferSubmitInfo commands{.commandBuffer = cmd};
    const vk::SubmitInfo2 submit{
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commands,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    return Submit(submit) && g_swapchain->Present(index);
}

// The text for the builds' progress and the dots' step, or none once every build has returned.
std::string Text(u32 step) {
    const BuildProgress progress = PipelineBuildProgress();
    if (!progress.building) {
        return {};
    }
    // The last percent waits for the builds that are still out.
    const u32 percent = std::min(99u, progress.made * 100 / std::max(1u, progress.expected));
    return fmt::format("Compiling shaders {}%{}", percent, Dots[step / 2 % Dots.size()]);
}

void Run() {
    Common::SetCurrentThreadName("GR2port Splash");
    u32 step = 0;
    std::unique_lock lock{g_mutex};
    while (!g_wake.wait_for(lock, Period, [] { return g_stop; })) {
        lock.unlock();
        std::string text = g_box.extent.width != 0 ? Text(step++) : std::string{};
        if (!text.empty() && text != g_text) {
            Compose(text);
            g_text_changed = true;
        }
        g_text = std::move(text);
        if (!Present(true)) {
            // The window changed: the swapchain and the label follow its size.
            const Window::Size size = Window::PixelSize();
            if (size.width != 0 && size.height != 0 &&
                g_swapchain->Create(size.width, size.height)) {
                Layout(g_swapchain->Extent());
            }
        }
        lock.lock();
    }
}

void OpenFont() {
    u32 subfont = 0;
    g_font = Libraries::Font::Internal::LoadBuiltinFontBytesShared(Font, &subfont);
    if (g_font == nullptr || g_font->empty() || FT_Init_FreeType(&g_library) != 0) {
        g_library = nullptr;
        return;
    }
    if (FT_New_Memory_Face(g_library, g_font->data(), static_cast<FT_Long>(g_font->size()),
                           static_cast<FT_Long>(subfont), &g_face) != 0) {
        g_face = nullptr;
    }
}

} // namespace

void Show(const Device& d, Memory& memory, Swapchain& swapchain, vk::CommandPool pool,
          vk::CommandBuffer cmd, vk::Fence done, vk::Semaphore acquired) {
    const Window::Size window = Window::PixelSize();
    if (window.width == 0 || window.height == 0 || !swapchain.Create(window.width, window.height)) {
        LOG_INFO(Render_Vulkan, "GR2port: no splash");
        return;
    }
    g_device = &d;
    g_memory = &memory;
    g_swapchain = &swapchain;
    g_pool = pool;
    g_commands = cmd;
    g_done = done;
    g_acquired = acquired;
    const auto& dl = d.dispatch;

    const std::vector<u8>& png = Common::ElfInfo::Instance().GetSplashData();
    int width = 0;
    int height = 0;
    stbi_uc* const pixels = png.empty()
                                ? nullptr
                                : stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                                        &width, &height, nullptr, 4);
    if (pixels != nullptr) {
        g_picture_extent = vk::Extent2D{static_cast<u32>(width), static_cast<u32>(height)};
        const u64 bytes = u64{g_picture_extent.width} * g_picture_extent.height * 4;
        g_pixels.assign(pixels, pixels + bytes);
        stbi_image_free(pixels);
        Buffer staging =
            memory.CreateBuffer(bytes, vk::BufferUsageFlagBits::eTransferSrc, Placement::Staging);
        // Sampled only because CreateImage makes a view, which a transfer-only image may not
        // have.
        g_picture = memory.CreateImage(PictureFormat, g_picture_extent,
                                       vk::ImageUsageFlagBits::eTransferDst |
                                           vk::ImageUsageFlagBits::eTransferSrc |
                                           vk::ImageUsageFlagBits::eSampled,
                                       vk::ImageAspectFlagBits::eColor);
        if (staging.mapped != nullptr && g_picture.image) {
            std::memcpy(staging.mapped, g_pixels.data(), bytes);
            const vk::CommandBuffer upload = Begin();
            Barrier(upload, g_picture.image, vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eNone,
                    vk::AccessFlagBits2::eTransferWrite);
            const vk::BufferImageCopy copy{
                .imageSubresource = Layers,
                .imageExtent = {g_picture_extent.width, g_picture_extent.height, 1},
            };
            upload.copyBufferToImage(staging.buffer, g_picture.image,
                                     vk::ImageLayout::eTransferDstOptimal, copy, dl);
            Barrier(upload, g_picture.image, vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferWrite,
                    vk::AccessFlagBits2::eTransferRead);
            (void)upload.end(dl);
            const vk::CommandBufferSubmitInfo commands{.commandBuffer = upload};
            Submit({.commandBufferInfoCount = 1, .pCommandBufferInfos = &commands});
        } else {
            memory.Destroy(g_picture);
            g_pixels.clear();
        }
        memory.Destroy(staging);
    }

    g_label = memory.CreateImage(PictureFormat, LabelCapacity,
                                 vk::ImageUsageFlagBits::eTransferDst |
                                     vk::ImageUsageFlagBits::eTransferSrc |
                                     vk::ImageUsageFlagBits::eSampled,
                                 vk::ImageAspectFlagBits::eColor);
    g_label_staging =
        memory.CreateBuffer(u64{LabelCapacity.width} * LabelCapacity.height * 4,
                            vk::BufferUsageFlagBits::eTransferSrc, Placement::Staging);
    OpenFont();
    Layout(swapchain.Extent());
    Present(false);
    g_shown = true;
    g_stop = false;
    g_thread = std::thread{Run};
    LOG_INFO(Render_Vulkan, "GR2port: splash {}x{} shown{}", g_picture_extent.width,
             g_picture_extent.height, g_box.extent.width != 0 ? "" : ", without the builds' text");
}

void End() {
    if (!g_shown) {
        return;
    }
    {
        std::scoped_lock lock{g_mutex};
        g_stop = true;
    }
    g_wake.notify_all();
    g_thread.join();
    g_shown = false;
    g_memory->Destroy(g_picture);
    g_memory->Destroy(g_label);
    g_memory->Destroy(g_label_staging);
    if (g_face != nullptr) {
        FT_Done_Face(g_face);
        g_face = nullptr;
    }
    if (g_library != nullptr) {
        FT_Done_FreeType(g_library);
        g_library = nullptr;
    }
    g_font.reset();
    g_pixels = {};
    g_under = {};
    g_coverage = {};
    g_rim = {};
    g_glyphs = {};
    g_label_written = false;
    g_box = vk::Rect2D{};
}

} // namespace Gr2Port::Splash
