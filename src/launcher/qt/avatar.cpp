// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <system_error>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>

#include "common/path_util.h"
#include "launcher/qt/avatar.h"

namespace Gr2Port::Launcher::Avatar {

std::filesystem::path Path(int user_id) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "gr2_avatar.png";
}

QImage Make(const std::filesystem::path& source) {
    QImageReader reader{QString::fromStdString(source.string())};
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    if (image.isNull() || image.width() == 0 || image.height() == 0) {
        return {};
    }
    const int side = std::min(image.width(), image.height());
    return image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
        .scaled(Side, Side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_ARGB32);
}

bool Save(const QImage& picture, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    QSaveFile file{QString::fromStdString(path.string())};
    if (!file.open(QIODevice::WriteOnly) || !picture.save(&file, "PNG")) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QPixmap Preview(const QImage& picture, int size, qreal ratio) {
    QPixmap circle{qRound(size * ratio), qRound(size * ratio)};
    circle.setDevicePixelRatio(ratio);
    circle.fill(Qt::white);
    QPainter painter{&circle};
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    const QRectF area{0.0, 0.0, static_cast<qreal>(size), static_cast<qreal>(size)};
    QPainterPath outline;
    outline.addEllipse(area);
    if (picture.isNull()) {
        painter.fillPath(outline, QColor{217, 217, 217});
    } else {
        painter.setClipPath(outline);
        painter.drawImage(area, picture);
    }
    return circle;
}

} // namespace Gr2Port::Launcher::Avatar
