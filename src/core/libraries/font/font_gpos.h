// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include "common/types.h"

// Pair kerning from the GPOS table of an OpenType font. A font without a kern table keeps its
// kerning here, and FT_Get_Kerning does not read it.
namespace Libraries::Font::Internal {

// The adjustment of one glyph pair in font units.
struct GposPairAdjustment {
    s16 advance_x; // Added to the advance of the first glyph.
    s16 advance_y;
    s16 placement_x; // Added to the position of the second glyph.
    s16 placement_y;
};

// Returns the offset of the kern feature of the latn script's default language in the table, or
// 0 if the table has none.
u32 FindGposKernFeature(std::span<const u8> gpos);

// Looks the pair up in the pair adjustment subtables of the feature, in the order of the table.
// Returns false if no subtable has the pair.
bool GetGposPairAdjustment(std::span<const u8> gpos, u32 feature, u32 first_glyph, u32 second_glyph,
                           GposPairAdjustment& adjustment);

} // namespace Libraries::Font::Internal
