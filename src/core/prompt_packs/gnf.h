// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "common/types.h"

// GNF texture files of Gravity Rush 2's UI: a 0x100-byte header with the texture's T#, then the
// texture data, RGBA8 or BC7 in the 1D thin micro-tiled layout. Images here are RGBA8 rows as the
// shaders see the texture, with the T#'s channel swizzle applied.
namespace PromptPacks::Gnf {

struct Texture {
    u32 width{};
    u32 height{};
    u32 pitch{};
    u32 data_format{};
    u32 tiling{};
    // Source channel of each output channel: 4..7 for R, G, B, A.
    std::array<u32, 4> swizzle{};
    u64 data_offset{};
    u64 data_size{};
};

/// The first texture of a GNF file; none when the file is not a single-mip RGBA8 or BC7 Color2D
/// texture in the 1D thin tiling.
std::optional<Texture> Parse(std::span<const u8> file);

/// The texture's pixels, width x height x 4 bytes.
std::vector<u8> Decode(std::span<const u8> file, const Texture& texture);

/// Writes image into the texture data of file. BC7 blocks whose pixels the image leaves as they
/// decode keep their bytes; the others are encoded again.
void Encode(std::span<u8> file, const Texture& texture, std::span<const u8> image);

/// BC7: one 4x4 block to 16 RGBA texels, and 16 RGBA texels to one block.
std::array<u8, 64> DecodeBc7(std::span<const u8, 16> block);
std::array<u8, 16> EncodeBc7(std::span<const u8, 64> texels);

} // namespace PromptPacks::Gnf
