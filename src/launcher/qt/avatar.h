// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <QImage>
#include <QPixmap>

// The profile picture Gravity Rush 2 sends to its server when it signs in to shadNet, kept as
// the server keeps it: the centre square of the chosen image, 128x128, as PNG.
namespace Gr2Port::Launcher::Avatar {

inline constexpr int Side = 128;

/// userdata/gr2_avatar.png, the player's picture, which the game uploads for any user.
std::filesystem::path Path(int user_id);

/// The picture made from the image at source, turned as its EXIF data says; null when the file
/// does not load.
QImage Make(const std::filesystem::path& source);

/// Writes picture to path as PNG, replacing the old file only once the new one is complete.
bool Save(const QImage& picture, const std::filesystem::path& path);

/// The picture in a circle size pixels across on white, or a grey circle for a null picture.
/// The pixmap holds ratio device pixels for each pixel.
QPixmap Preview(const QImage& picture, int size, qreal ratio);

} // namespace Gr2Port::Launcher::Avatar
