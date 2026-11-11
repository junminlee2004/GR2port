// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include "core/libraries/font/font_gpos.h"

namespace Libraries::Font::Internal {

namespace {

constexpr u32 LookupPairAdjustment = 2;
constexpr u32 LookupExtension = 9;

constexpr u32 Tag(char a, char b, char c, char d) {
    return static_cast<u32>(static_cast<u8>(a)) << 24 | static_cast<u32>(static_cast<u8>(b)) << 16 |
           static_cast<u32>(static_cast<u8>(c)) << 8 | static_cast<u8>(d);
}

// Big endian reads. A read past the end of the table gives 0, which the callers take as an empty
// list or an unknown format.
u32 Read16(std::span<const u8> table, size_t offset) {
    if (offset + 2 > table.size()) {
        return 0;
    }
    return static_cast<u32>(table[offset]) << 8 | table[offset + 1];
}

u32 Read32(std::span<const u8> table, size_t offset) {
    return Read16(table, offset) << 16 | Read16(table, offset + 2);
}

// Returns the coverage index of the glyph, or -1 if the coverage table does not list it.
s32 CoverageIndex(std::span<const u8> table, size_t coverage, u32 glyph) {
    const u32 format = Read16(table, coverage);
    s32 low = 0;
    s32 high = static_cast<s32>(Read16(table, coverage + 2)) - 1;
    while (low <= high) {
        const s32 mid = (low + high) / 2;
        if (format == 1) {
            const u32 listed = Read16(table, coverage + 4 + mid * 2);
            if (glyph == listed) {
                return mid;
            }
            if (glyph < listed) {
                high = mid - 1;
            } else {
                low = mid + 1;
            }
        } else if (format == 2) {
            const size_t range = coverage + 4 + mid * 6;
            const u32 first = Read16(table, range);
            if (glyph < first) {
                high = mid - 1;
            } else if (glyph > Read16(table, range + 2)) {
                low = mid + 1;
            } else {
                return static_cast<s32>(Read16(table, range + 4) + glyph - first);
            }
        } else {
            break;
        }
    }
    return -1;
}

// Returns the class of the glyph. A glyph the class table does not list has class 0.
u32 ClassOf(std::span<const u8> table, size_t class_def, u32 glyph) {
    const u32 format = Read16(table, class_def);
    if (format == 1) {
        const u32 first = Read16(table, class_def + 2);
        if (glyph >= first && glyph - first < Read16(table, class_def + 4)) {
            return Read16(table, class_def + 6 + (glyph - first) * 2);
        }
    } else if (format == 2) {
        s32 low = 0;
        s32 high = static_cast<s32>(Read16(table, class_def + 2)) - 1;
        while (low <= high) {
            const s32 mid = (low + high) / 2;
            const size_t range = class_def + 4 + mid * 6;
            if (glyph < Read16(table, range)) {
                high = mid - 1;
            } else if (glyph > Read16(table, range + 2)) {
                low = mid + 1;
            } else {
                return Read16(table, range + 4);
            }
        }
    }
    return 0;
}

// Reads the two value records of a pair. Each record holds only the fields its format lists, in
// the order x placement, y placement, x advance, y advance, then four device table offsets.
void ReadPairValues(std::span<const u8> table, size_t record, u32 format1, u32 format2,
                    GposPairAdjustment& adjustment) {
    const auto next = [&](u32 present) -> s16 {
        if (present == 0) {
            return 0;
        }
        const auto value = static_cast<s16>(Read16(table, record));
        record += 2;
        return value;
    };
    next(format1 & 1);
    next(format1 & 2);
    adjustment.advance_x = next(format1 & 4);
    adjustment.advance_y = next(format1 & 8);
    record += std::popcount(format1 & 0xf0) * 2;
    adjustment.placement_x = next(format2 & 1);
    adjustment.placement_y = next(format2 & 2);
}

// Looks the pair up in one pair adjustment subtable. A class subtable has every pair whose first
// glyph it covers. A pair list subtable has only the pairs it lists.
bool GetPairAdjustment(std::span<const u8> table, size_t subtable, u32 first_glyph,
                       u32 second_glyph, GposPairAdjustment& adjustment) {
    const u32 format = Read16(table, subtable);
    const u32 coverage = Read16(table, subtable + 2);
    if ((format != 1 && format != 2) || coverage == 0) {
        return false;
    }
    const s32 index = CoverageIndex(table, subtable + coverage, first_glyph);
    if (index < 0) {
        return false;
    }
    const u32 format1 = Read16(table, subtable + 4);
    const u32 format2 = Read16(table, subtable + 6);
    const u32 values_size = (std::popcount(format1) + std::popcount(format2)) * 2;
    if (format == 1) {
        if (static_cast<u32>(index) >= Read16(table, subtable + 8)) {
            return false;
        }
        const size_t pair_set = subtable + Read16(table, subtable + 10 + index * 2);
        const u32 pair_count = Read16(table, pair_set);
        for (u32 i = 0; i < pair_count; ++i) {
            const size_t pair = pair_set + 2 + i * (values_size + 2);
            if (Read16(table, pair) == second_glyph) {
                ReadPairValues(table, pair + 2, format1, format2, adjustment);
                return true;
            }
        }
        return false;
    }
    const u32 class_def1 = Read16(table, subtable + 8);
    const u32 class_def2 = Read16(table, subtable + 10);
    const u32 class2_count = Read16(table, subtable + 14);
    const u32 class2 = class_def2 != 0 ? ClassOf(table, subtable + class_def2, second_glyph) : 0;
    const u32 class1 = class_def1 != 0 ? ClassOf(table, subtable + class_def1, first_glyph) : 0;
    if (class2 >= class2_count || class1 >= Read16(table, subtable + 12)) {
        return false;
    }
    ReadPairValues(table, subtable + 16 + (class1 * class2_count + class2) * values_size, format1,
                   format2, adjustment);
    return true;
}

} // namespace

u32 FindGposKernFeature(std::span<const u8> gpos) {
    const u32 script_list = Read16(gpos, 4);
    const u32 feature_list = Read16(gpos, 6);
    const u32 feature_count = Read16(gpos, feature_list);
    const auto kern_feature = [&](u32 index) -> u32 {
        const size_t record = feature_list + 2 + index * 6;
        if (index >= feature_count || Read32(gpos, record) != Tag('k', 'e', 'r', 'n')) {
            return 0;
        }
        return feature_list + Read16(gpos, record + 4);
    };
    const u32 script_count = Read16(gpos, script_list);
    for (u32 i = 0; i < script_count; ++i) {
        const size_t record = script_list + 2 + i * 6;
        if (Read32(gpos, record) != Tag('l', 'a', 't', 'n')) {
            continue;
        }
        const size_t script = script_list + Read16(gpos, record + 4);
        // The default language system, or the first listed one if the script has no default.
        u32 lang_sys = Read16(gpos, script);
        if (lang_sys == 0) {
            if (Read16(gpos, script + 2) == 0) {
                return 0;
            }
            lang_sys = Read16(gpos, script + 8);
        }
        const size_t language = script + lang_sys;
        // The required feature goes first, then the features in the order of the language system.
        if (const u32 feature = kern_feature(Read16(gpos, language + 2))) {
            return feature;
        }
        const u32 index_count = Read16(gpos, language + 4);
        for (u32 j = 0; j < index_count; ++j) {
            if (const u32 feature = kern_feature(Read16(gpos, language + 6 + j * 2))) {
                return feature;
            }
        }
        return 0;
    }
    return 0;
}

bool GetGposPairAdjustment(std::span<const u8> gpos, u32 feature, u32 first_glyph, u32 second_glyph,
                           GposPairAdjustment& adjustment) {
    const u32 lookup_list = Read16(gpos, 8);
    const u32 lookup_total = Read16(gpos, lookup_list);
    const u32 lookup_count = Read16(gpos, feature + 2);
    for (u32 i = 0; i < lookup_count; ++i) {
        const u32 index = Read16(gpos, feature + 4 + i * 2);
        const u32 lookup_offset = Read16(gpos, lookup_list + 2 + index * 2);
        if (index >= lookup_total || lookup_offset == 0) {
            return false;
        }
        const size_t lookup = lookup_list + lookup_offset;
        const u32 lookup_type = Read16(gpos, lookup);
        const u32 subtable_count = Read16(gpos, lookup + 4);
        for (u32 j = 0; j < subtable_count; ++j) {
            const u32 subtable_offset = Read16(gpos, lookup + 6 + j * 2);
            if (subtable_offset == 0) {
                break;
            }
            size_t subtable = lookup + subtable_offset;
            u32 type = lookup_type;
            if (type == LookupExtension) {
                // An extension subtable holds the real type and a 32-bit offset to the subtable.
                if (Read16(gpos, subtable) != 1) {
                    continue;
                }
                type = Read16(gpos, subtable + 2);
                subtable += Read32(gpos, subtable + 4);
            }
            if (type == LookupPairAdjustment &&
                GetPairAdjustment(gpos, subtable, first_glyph, second_glyph, adjustment)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace Libraries::Font::Internal
