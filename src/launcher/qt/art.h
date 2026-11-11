// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QRectF>

class QPainter;

namespace Gr2Port::Launcher {

// The launcher's pictures: the game's splash without its letterbox, and its logo with the glow.
class Art {
public:
    /// Decodes the pictures. A picture that does not decode draws nothing.
    Art();

    /// The splash's size in pixels.
    float SplashWidth() const;
    float SplashHeight() const;

    /// Draws part, in splash pixels, over dest.
    void DrawSplash(QPainter& painter, const QRectF& dest, const QRectF& part) const;

    /// The logo's width over its height.
    float LogoAspect() const;

    void DrawLogo(QPainter& painter, const QRectF& dest) const;

    /// The game's icon for the launcher's windows.
    const QIcon& Icon() const;

    /// Darkens rect from clear at its left edge to opacity at its right edge.
    static void Fade(QPainter& painter, const QRectF& rect, int opacity);

    /// Runs draw on a black picture of width x height pixels at 96 DPI, which holds ratio device
    /// pixels for each of them.
    static QPixmap Render(int width, int height, qreal ratio,
                          const std::function<void(QPainter&)>& draw);

private:
    QImage splash;
    QImage logo;
    QIcon icon;
};

} // namespace Gr2Port::Launcher
