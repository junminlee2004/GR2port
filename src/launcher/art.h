// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <windows.h>
// windows.h first
#include <objidl.h>

// GDI+ names min and max unqualified, and NOMINMAX keeps windows.h from defining them.
namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

namespace Gr2Port::Launcher {

// The launcher's pictures: the game's splash without its letterbox, and its logo with the glow.
class Art {
public:
    /// Starts GDI+ and decodes the pictures. A picture that does not decode draws nothing.
    Art();
    ~Art();

    Art(const Art&) = delete;
    Art& operator=(const Art&) = delete;

    /// The splash's size in pixels.
    float SplashWidth() const;
    float SplashHeight() const;

    /// Draws part, in splash pixels, over dest.
    void DrawSplash(Gdiplus::Graphics& graphics, const Gdiplus::RectF& dest,
                    const Gdiplus::RectF& part) const;

    /// The logo's width over its height.
    float LogoAspect() const;

    void DrawLogo(Gdiplus::Graphics& graphics, const Gdiplus::RectF& dest) const;

    /// Darkens rect from clear at its left edge to opacity at its right edge.
    static void Fade(Gdiplus::Graphics& graphics, const Gdiplus::RectF& rect, BYTE opacity);

    /// Runs draw on a width x height picture and returns it as a bitmap for GDI, which the caller
    /// deletes. Null when GDI+ fails.
    static HBITMAP Render(int width, int height,
                          const std::function<void(Gdiplus::Graphics&)>& draw);

private:
    ULONG_PTR token{};
    std::unique_ptr<Gdiplus::Bitmap> splash;
    std::unique_ptr<Gdiplus::Bitmap> logo;
};

} // namespace Gr2Port::Launcher
