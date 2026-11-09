// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>

#include "launcher/art.h"

// The profile picture Gravity Rush 2 sends to its server when it signs in to shadNet, kept as
// the server keeps it: the centre square of the chosen image, 128x128, as PNG.
namespace Gr2Port::Launcher::Avatar {

inline constexpr int Side = 128;

/// <home>/<user id>/gr2_avatar.png, which the game uploads for that user.
std::filesystem::path Path(int user_id);

/// The picture made from the image at source, turned as its EXIF data says; null when the file
/// does not load.
std::unique_ptr<Gdiplus::Bitmap> Make(const std::filesystem::path& source);

/// Writes picture to path as PNG, replacing the old file only once the new one is complete.
bool Save(Gdiplus::Bitmap& picture, const std::filesystem::path& path);

/// The picture in a circle size pixels across on white, or a grey circle without one.
HBITMAP Preview(Gdiplus::Bitmap* picture, int size);

} // namespace Gr2Port::Launcher::Avatar
