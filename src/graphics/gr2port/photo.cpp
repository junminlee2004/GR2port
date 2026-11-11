// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/stb.h"
#include "graphics/gr2port/photo.h"
#include "graphics/gr2port/ui.h"

namespace Gr2Port::Photo {

namespace {

constexpr u32 NoSlot = ~0u;
constexpr u32 PixelBytes = 4;
// The encoder runs two game frames after the shot; the renderer is at most a few frames behind.
constexpr auto ReadWait = std::chrono::seconds(2);

// The render thread's copy in flight.
Buffer g_buffer;
u32 g_slot = NoSlot;
u64 g_frame = 0;
vk::Extent2D g_extent;

// The newest photo, in RGBA order, and the frame of the one the encoder took last.
std::mutex g_mutex;
std::condition_variable g_delivered_cv;
std::vector<u8> g_pixels;
vk::Extent2D g_size;
u64 g_delivered = 0;
u64 g_taken = 0;

// Writes a photo as pt_photo.png in the log folder, off the encoder's thread.
void SavePicture(std::vector<u8> pixels, vk::Extent2D extent) {
    std::thread([pixels = std::move(pixels), extent]() mutable {
        for (size_t i = 0; i < pixels.size(); i += PixelBytes) {
            pixels[i + 3] = 0xff;
        }
        std::vector<u8> png;
        stbi_write_png_to_func(
            [](void* context, void* data, int size) {
                auto* out = static_cast<std::vector<u8>*>(context);
                const auto* bytes = static_cast<const u8*>(data);
                out->insert(out->end(), bytes, bytes + size);
            },
            &png, static_cast<int>(extent.width), static_cast<int>(extent.height), PixelBytes,
            pixels.data(), static_cast<int>(extent.width * PixelBytes));
        const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "pt_photo.png";
        Common::FS::IOFile file{path, Common::FS::FileAccessMode::Create};
        if (file.IsOpen()) {
            file.WriteRaw<u8>(png.data(), png.size());
        }
    }).detach();
}

} // namespace

void Record(Memory& memory, vk::CommandBuffer cmd, const Scene::Header& header, u32 slot) {
    if (header.ui.photo == 0 || g_slot != NoSlot) {
        return;
    }
    const u32 target = header.ui.photo - 1;
    const vk::Extent2D extent = Ui::TargetExtent(target);
    const u64 bytes = u64{extent.width} * extent.height * PixelBytes;
    if (bytes == 0) {
        return;
    }
    if (g_buffer.size < bytes) {
        memory.Destroy(g_buffer);
        g_buffer =
            memory.CreateBuffer(bytes, vk::BufferUsageFlagBits::eTransferDst, Placement::Readback);
        if (g_buffer.mapped == nullptr) {
            LOG_ERROR(Render_Vulkan, "GR2port photo: no memory to read back {}x{}", extent.width,
                      extent.height);
            return;
        }
    }
    Ui::CopyTarget(cmd, target, g_buffer.buffer);
    g_slot = slot;
    g_frame = header.frame;
    g_extent = extent;
}

void Collect(u32 slot) {
    if (g_slot != slot) {
        return;
    }
    g_slot = NoSlot;
    {
        std::scoped_lock lock{g_mutex};
        g_pixels.assign(g_buffer.mapped,
                        g_buffer.mapped + u64{g_extent.width} * g_extent.height * PixelBytes);
        g_size = g_extent;
        g_delivered = g_frame;
    }
    g_delivered_cv.notify_all();
    LOG_INFO(Render_Vulkan, "GR2port photo: frame {} read back, {}x{}", g_frame, g_extent.width,
             g_extent.height);
}

bool Read(std::vector<u8>& pixels, u32& width, u32& height, u32& pitch) {
    std::unique_lock lock{g_mutex};
    if (!g_delivered_cv.wait_for(lock, ReadWait, [] { return g_delivered > g_taken; })) {
        LOG_WARNING(Render_Vulkan, "GR2port photo: no photo drawn for the encoder");
        return false;
    }
    g_taken = g_delivered;
    SavePicture(g_pixels, g_size);
    width = g_size.width;
    height = g_size.height;
    pitch = width * PixelBytes;
    pixels.resize(g_pixels.size());
    // The target holds R8G8B8A8; the game encodes B8G8R8A8.
    for (u64 i = 0; i < g_pixels.size(); i += PixelBytes) {
        pixels[i] = g_pixels[i + 2];
        pixels[i + 1] = g_pixels[i + 1];
        pixels[i + 2] = g_pixels[i];
        pixels[i + 3] = g_pixels[i + 3];
    }
    return true;
}

} // namespace Gr2Port::Photo
