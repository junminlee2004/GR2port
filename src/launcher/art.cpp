// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <windows.h>
// windows.h first
#include <cmrc/cmrc.hpp>
#include <shlwapi.h>

#include "common/logging/log.h"
#include "launcher/art.h"

CMRC_DECLARE(launcher);

namespace Gr2Port::Launcher {

namespace {

// Decodes an embedded picture into memory of its own, since GDI+ reads a stream lazily.
std::unique_ptr<Gdiplus::Bitmap> Decode(const char* name) {
    const cmrc::embedded_filesystem files = cmrc::launcher::get_filesystem();
    if (!files.exists(name)) {
        return nullptr;
    }
    const cmrc::file file = files.open(name);
    IStream* const stream = SHCreateMemStream(reinterpret_cast<const BYTE*>(file.begin()),
                                              static_cast<UINT>(file.size()));
    if (stream == nullptr) {
        return nullptr;
    }
    std::unique_ptr<Gdiplus::Bitmap> decoded{Gdiplus::Bitmap::FromStream(stream)};
    std::unique_ptr<Gdiplus::Bitmap> copy;
    if (decoded != nullptr && decoded->GetLastStatus() == Gdiplus::Ok) {
        const INT width = static_cast<INT>(decoded->GetWidth());
        const INT height = static_cast<INT>(decoded->GetHeight());
        copy = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
        Gdiplus::Graphics graphics{copy.get()};
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics.DrawImage(decoded.get(), 0, 0, width, height);
    }
    decoded.reset();
    stream->Release();
    if (copy == nullptr || copy->GetLastStatus() != Gdiplus::Ok) {
        LOG_ERROR(Frontend, "GR2port: the launcher cannot decode {}", name);
        return nullptr;
    }
    return copy;
}

} // namespace

Art::Art() {
    const Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        LOG_ERROR(Frontend, "GR2port: GDI+ does not start");
        token = 0;
        return;
    }
    splash = Decode("src/resources/launcher/splash.jpg");
    logo = Decode("src/resources/launcher/logo.png");
}

Art::~Art() {
    splash.reset();
    logo.reset();
    if (token != 0) {
        Gdiplus::GdiplusShutdown(token);
    }
}

float Art::SplashWidth() const {
    return splash ? static_cast<float>(splash->GetWidth()) : 1.0f;
}

float Art::SplashHeight() const {
    return splash ? static_cast<float>(splash->GetHeight()) : 1.0f;
}

void Art::DrawSplash(Gdiplus::Graphics& graphics, const Gdiplus::RectF& dest,
                     const Gdiplus::RectF& part) const {
    if (splash == nullptr) {
        return;
    }
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    graphics.DrawImage(splash.get(), dest, part.X, part.Y, part.Width, part.Height,
                       Gdiplus::UnitPixel, &attributes);
}

float Art::LogoAspect() const {
    return logo ? static_cast<float>(logo->GetWidth()) / static_cast<float>(logo->GetHeight())
                : 1.0f;
}

void Art::DrawLogo(Gdiplus::Graphics& graphics, const Gdiplus::RectF& dest) const {
    if (logo == nullptr) {
        return;
    }
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    graphics.DrawImage(logo.get(), dest, 0.0f, 0.0f, static_cast<float>(logo->GetWidth()),
                       static_cast<float>(logo->GetHeight()), Gdiplus::UnitPixel, &attributes);
}

void Art::Fade(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect, BYTE opacity) {
    const Gdiplus::LinearGradientBrush brush{
        Gdiplus::PointF{rect.X - 1.0f, 0.0f}, Gdiplus::PointF{rect.GetRight() + 1.0f, 0.0f},
        Gdiplus::Color{0, 0, 0, 0}, Gdiplus::Color{opacity, 0, 0, 0}};
    graphics.FillRectangle(&brush, rect);
}

HBITMAP Art::Render(int width, int height, const std::function<void(Gdiplus::Graphics&)>& draw) {
    Gdiplus::Bitmap picture{width, height, PixelFormat32bppPARGB};
    {
        Gdiplus::Graphics graphics{&picture};
        graphics.Clear(Gdiplus::Color{255, 0, 0, 0});
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        draw(graphics);
    }
    HBITMAP bitmap = nullptr;
    if (picture.GetHBITMAP(Gdiplus::Color{255, 0, 0, 0}, &bitmap) != Gdiplus::Ok) {
        return nullptr;
    }
    return bitmap;
}

} // namespace Gr2Port::Launcher
