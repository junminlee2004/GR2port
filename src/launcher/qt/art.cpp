// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QLinearGradient>
#include <QPainter>
#include <cmrc/cmrc.hpp>

#include "common/logging/log.h"
#include "launcher/qt/art.h"

CMRC_DECLARE(launcher);

namespace Gr2Port::Launcher {

namespace {

QImage Decode(const char* name) {
    const cmrc::embedded_filesystem files = cmrc::launcher::get_filesystem();
    if (!files.exists(name)) {
        return {};
    }
    const cmrc::file file = files.open(name);
    QImage image = QImage::fromData(reinterpret_cast<const uchar*>(file.begin()),
                                    static_cast<int>(file.size()));
    if (image.isNull()) {
        LOG_ERROR(Frontend, "GR2port: the launcher cannot decode {}", name);
        return {};
    }
    return image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

// Draws part of image over dest. The part is scaled to dest's device pixels first, which keeps
// a picture that shrinks a lot smooth.
void DrawPart(QPainter& painter, const QImage& image, const QRectF& dest, const QRectF& part) {
    const qreal ratio = painter.device()->devicePixelRatioF();
    const QSize size{qMax(1, qRound(dest.width() * ratio)), qMax(1, qRound(dest.height() * ratio))};
    const QImage scaled =
        image.copy(part.toRect()).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    painter.drawImage(dest, scaled);
}

} // namespace

Art::Art() {
    splash = Decode("src/resources/launcher/splash.jpg");
    logo = Decode("src/resources/launcher/logo.png");
    icon = QIcon{QPixmap::fromImage(Decode("src/dist/gravity_rush_2.ico"))};
}

float Art::SplashWidth() const {
    return splash.isNull() ? 1.0f : static_cast<float>(splash.width());
}

float Art::SplashHeight() const {
    return splash.isNull() ? 1.0f : static_cast<float>(splash.height());
}

void Art::DrawSplash(QPainter& painter, const QRectF& dest, const QRectF& part) const {
    if (!splash.isNull()) {
        DrawPart(painter, splash, dest, part);
    }
}

float Art::LogoAspect() const {
    return logo.isNull() ? 1.0f
                         : static_cast<float>(logo.width()) / static_cast<float>(logo.height());
}

void Art::DrawLogo(QPainter& painter, const QRectF& dest) const {
    if (!logo.isNull()) {
        DrawPart(painter, logo, dest, QRectF{logo.rect()});
    }
}

const QIcon& Art::Icon() const {
    return icon;
}

void Art::Fade(QPainter& painter, const QRectF& rect, int opacity) {
    QLinearGradient gradient{QPointF{rect.left() - 1.0, 0.0}, QPointF{rect.right() + 1.0, 0.0}};
    gradient.setColorAt(0.0, QColor{0, 0, 0, 0});
    gradient.setColorAt(1.0, QColor{0, 0, 0, opacity});
    painter.fillRect(rect, gradient);
}

QPixmap Art::Render(int width, int height, qreal ratio,
                    const std::function<void(QPainter&)>& draw) {
    QPixmap picture{qRound(width * ratio), qRound(height * ratio)};
    picture.setDevicePixelRatio(ratio);
    picture.fill(Qt::black);
    QPainter painter{&picture};
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    draw(painter);
    return picture;
}

} // namespace Gr2Port::Launcher
