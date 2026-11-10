// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/path_util.h"
#include "launcher/avatar.h"

namespace Gr2Port::Launcher::Avatar {

namespace {

constexpr PROPID OrientationTag = 0x0112;

// EXIF orientations 2 to 8 as the turn and flip that put the image upright.
void Upright(Gdiplus::Bitmap& image) {
    const UINT size = image.GetPropertyItemSize(OrientationTag);
    if (size == 0) {
        return;
    }
    std::vector<char> bytes(size);
    auto* item = reinterpret_cast<Gdiplus::PropertyItem*>(bytes.data());
    if (image.GetPropertyItem(OrientationTag, size, item) != Gdiplus::Ok ||
        item->type != PropertyTagTypeShort || item->length < sizeof(USHORT)) {
        return;
    }
    constexpr Gdiplus::RotateFlipType Turns[] = {
        Gdiplus::RotateNoneFlipNone, Gdiplus::RotateNoneFlipX,   Gdiplus::Rotate180FlipNone,
        Gdiplus::RotateNoneFlipY,    Gdiplus::Rotate90FlipX,     Gdiplus::Rotate90FlipNone,
        Gdiplus::Rotate270FlipX,     Gdiplus::Rotate270FlipNone,
    };
    const USHORT orientation = *static_cast<const USHORT*>(item->value);
    if (orientation >= 2 && orientation <= 8) {
        image.RotateFlip(Turns[orientation - 1]);
    }
}

bool PngEncoder(CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) {
        return false;
    }
    std::vector<char> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
        return false;
    }
    for (UINT i = 0; i < count; i++) {
        if (std::wstring_view{encoders[i].MimeType} == L"image/png") {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

} // namespace

std::filesystem::path Path(int user_id) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "gr2_avatar.png";
}

std::unique_ptr<Gdiplus::Bitmap> Make(const std::filesystem::path& source) {
    Gdiplus::Bitmap image{source.c_str()};
    if (image.GetLastStatus() != Gdiplus::Ok || image.GetWidth() == 0 || image.GetHeight() == 0) {
        return nullptr;
    }
    Upright(image);
    const int width = static_cast<int>(image.GetWidth());
    const int height = static_cast<int>(image.GetHeight());
    const int side = std::min(width, height);
    auto picture = std::make_unique<Gdiplus::Bitmap>(Side, Side, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics{picture.get()};
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    // Edge pixels repeat inward instead of fading to transparent.
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    graphics.DrawImage(&image, Gdiplus::Rect{0, 0, Side, Side}, (width - side) / 2,
                       (height - side) / 2, side, side, Gdiplus::UnitPixel, &attributes);
    if (picture->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }
    return picture;
}

bool Save(Gdiplus::Bitmap& picture, const std::filesystem::path& path) {
    CLSID png{};
    if (!PngEncoder(png)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path written = path;
    written += L".new";
    if (picture.Save(written.c_str(), &png, nullptr) != Gdiplus::Ok) {
        std::filesystem::remove(written, ec);
        return false;
    }
    return MoveFileExW(written.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

HBITMAP Preview(Gdiplus::Bitmap* picture, int size) {
    Gdiplus::Bitmap circle{size, size, PixelFormat32bppARGB};
    Gdiplus::Graphics graphics{&circle};
    graphics.Clear(Gdiplus::Color{255, 255, 255, 255});
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    const Gdiplus::RectF area{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size)};
    if (picture != nullptr) {
        Gdiplus::TextureBrush brush{picture, Gdiplus::WrapModeClamp};
        brush.ScaleTransform(static_cast<float>(size) / Side, static_cast<float>(size) / Side);
        graphics.FillEllipse(&brush, area);
    } else {
        const Gdiplus::SolidBrush grey{Gdiplus::Color{255, 217, 217, 217}};
        graphics.FillEllipse(&grey, area);
    }
    HBITMAP bitmap = nullptr;
    circle.GetHBITMAP(Gdiplus::Color{255, 255, 255, 255}, &bitmap);
    return bitmap;
}

} // namespace Gr2Port::Launcher::Avatar
