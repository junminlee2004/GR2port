// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "core/prompt_packs/gnf.h"

namespace PromptPacks::Gnf {

namespace {

constexpr u32 HeaderTexture = 0x10;
constexpr u32 FormatRgba8 = 10;
constexpr u32 FormatBc7 = 41;
constexpr u32 TilingThin1D = 13;
constexpr u32 TypeColor2D = 9;

u32 Word(std::span<const u8> file, u32 index) {
    u32 value;
    std::memcpy(&value, file.data() + HeaderTexture + index * 4, sizeof(value));
    return value;
}

u32 Bits(u32 word, u32 bit, u32 width) {
    return (word >> bit) & ((1u << width) - 1);
}

struct Layout {
    u32 block;
    u32 element_bytes;
    u32 pitch;  // In elements, aligned.
    u32 height; // In elements, aligned.
};

Layout SurfaceLayout(const Texture& texture) {
    const bool bc7 = texture.data_format == FormatBc7;
    Layout layout{bc7 ? 4u : 1u, bc7 ? 16u : 4u, 0, 0};
    const u32 pitch = (texture.pitch + layout.block - 1) / layout.block;
    const u32 height = (texture.height + layout.block - 1) / layout.block;
    layout.pitch = (pitch + 7) & ~7u;
    layout.height = (height + 7) & ~7u;
    while ((static_cast<u64>(layout.pitch) * layout.height * layout.element_bytes) % 256 != 0) {
        layout.pitch += 8;
    }
    return layout;
}

// The element's index in the 1D thin micro-tiled buffer: 8x8 tiles in rows, and inside a tile the
// bits x0, y0, x1, y1, x2, y2.
u64 TiledIndex(const Layout& layout, u32 x, u32 y) {
    const u64 tile = static_cast<u64>(y >> 3) * (layout.pitch >> 3) + (x >> 3);
    const u32 micro = (x & 1) | (y & 1) << 1 | (x >> 1 & 1) << 2 | (y >> 1 & 1) << 3 |
                      (x >> 2 & 1) << 4 | (y >> 2 & 1) << 5;
    return tile * 64 + micro;
}

// ------------------------------------------------------------------------------------------- BC7
// Two bits per texel: the subset of each texel in the 2- and 3-subset partitions.
constexpr std::array<u32, 64> Partition2{
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
    0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
    0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
    0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
    0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
    0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
    0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404,
};
constexpr std::array<u32, 64> Partition3{
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};
// The anchor texel of subset 1 (2 subsets), and of subsets 1 and 2 (3 subsets).
constexpr std::array<u8, 64> Anchor2{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};
constexpr std::array<u8, 64> Anchor3First{
    3,  3,  15, 15, 8, 3,  15, 15, 8,  8,  6, 6,  6, 5,  3, 3,  3,  3,  8,  15, 3,  3,
    6,  10, 5,  8,  8, 6,  8,  5,  15, 15, 8, 15, 3, 5,  6, 10, 8,  15, 15, 3,  15, 5,
    15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5, 10, 5, 10, 8, 13, 15, 12, 3,  3,
};
constexpr std::array<u8, 64> Anchor3Second{
    15, 8, 8, 3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8, 3, 15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6, 8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr std::array<u8, 4> Weights2{0, 21, 43, 64};
constexpr std::array<u8, 8> Weights3{0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<u8, 16> Weights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Mode {
    u8 subsets;
    u8 partition_bits;
    u8 rotation_bits;
    u8 selection_bits;
    u8 color_bits;
    u8 alpha_bits;
    u8 endpoint_pbits;
    u8 shared_pbits;
    u8 index_bits;
    u8 index2_bits;
};

constexpr std::array<Mode, 8> Modes{{
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};

class BitReader {
public:
    explicit BitReader(std::span<const u8, 16> block) {
        std::memcpy(&low, block.data(), 8);
        std::memcpy(&high, block.data() + 8, 8);
    }

    u32 Read(u32 count) {
        u32 value = 0;
        for (u32 i = 0; i < count; ++i, ++position) {
            const u64 word = position < 64 ? low : high;
            value |= static_cast<u32>(word >> (position & 63) & 1) << i;
        }
        return value;
    }

private:
    u64 low{};
    u64 high{};
    u32 position{};
};

class BitWriter {
public:
    void Write(u32 value, u32 count) {
        for (u32 i = 0; i < count; ++i, ++position) {
            u64& word = position < 64 ? low : high;
            word |= static_cast<u64>(value >> i & 1) << (position & 63);
        }
    }

    std::array<u8, 16> Bytes() const {
        std::array<u8, 16> bytes{};
        std::memcpy(bytes.data(), &low, 8);
        std::memcpy(bytes.data() + 8, &high, 8);
        return bytes;
    }

private:
    u64 low{};
    u64 high{};
    u32 position{};
};

// Spreads a value of bits bits over 8 bits by repeating its top bits.
u8 Unquantize(u32 value, u32 bits) {
    if (bits >= 8) {
        return static_cast<u8>(value);
    }
    value <<= 8 - bits;
    return static_cast<u8>(value | value >> bits);
}

u8 Interpolate(u8 e0, u8 e1, u8 weight) {
    return static_cast<u8>(((64 - weight) * e0 + weight * e1 + 32) >> 6);
}

const u8* WeightTable(u32 bits) {
    return bits == 2 ? Weights2.data() : bits == 3 ? Weights3.data() : Weights4.data();
}

u32 SubsetOf(const Mode& mode, u32 partition, u32 texel) {
    if (mode.subsets == 2) {
        return Partition2[partition] >> (texel * 2) & 3;
    }
    if (mode.subsets == 3) {
        return Partition3[partition] >> (texel * 2) & 3;
    }
    return 0;
}

bool IsAnchor(const Mode& mode, u32 partition, u32 texel) {
    if (texel == 0) {
        return true;
    }
    if (mode.subsets == 2) {
        return texel == Anchor2[partition];
    }
    if (mode.subsets == 3) {
        return texel == Anchor3First[partition] || texel == Anchor3Second[partition];
    }
    return false;
}

// ------------------------------------------------------------------------------- BC7 encoder
// Mode 6 (one subset, RGBA 7 bits and a p-bit per endpoint, 4-bit indices) and mode 7 (two
// subsets of a partition, RGBA 5 bits and a p-bit per endpoint, 2-bit indices); a block takes the
// one with the least squared error.
struct Endpoint {
    std::array<u8, 4> value; // As the decoder expands it.
    std::array<u32, 4> code;
    u32 pbit;
};

struct SubsetFit {
    std::array<Endpoint, 2> ends;
    std::array<u8, 16> indices; // Of the subset's texels; others are 0.
    u64 error;
};

u64 TexelError(const u8* texel, const std::array<u8, 4>& color) {
    u64 error = 0;
    for (u32 c = 0; c < 4; ++c) {
        const int d = static_cast<int>(texel[c]) - color[c];
        error += static_cast<u64>(d * d);
    }
    return error;
}

// The nearest endpoint with bits stored bits per channel and one p-bit.
Endpoint Quantize(const std::array<float, 4>& value, u32 bits) {
    Endpoint best{};
    float best_error = 1e30f;
    const u32 top = (1u << bits) - 1;
    for (u32 p = 0; p < 2; ++p) {
        Endpoint candidate{{}, {}, p};
        float error = 0;
        for (u32 c = 0; c < 4; ++c) {
            const float v = std::clamp(value[c], 0.0f, 255.0f);
            const float scaled = v / 255.0f * static_cast<float>(top);
            const int guess = static_cast<int>(std::lround(scaled));
            float channel_error = 1e30f;
            for (int code = std::max(guess - 1, 0); code <= std::min(guess + 1, int(top)); ++code) {
                const u8 expanded = Unquantize(static_cast<u32>(code) << 1 | p, bits + 1);
                const float d = v - expanded;
                if (d * d < channel_error) {
                    channel_error = d * d;
                    candidate.code[c] = static_cast<u32>(code);
                    candidate.value[c] = expanded;
                }
            }
            error += channel_error;
        }
        if (error < best_error) {
            best_error = error;
            best = candidate;
        }
    }
    return best;
}

SubsetFit FitIndices(std::span<const u8, 64> texels, u32 mask, const std::array<Endpoint, 2>& ends,
                     u32 index_bits) {
    SubsetFit fit{ends, {}, 0};
    const u32 count = 1u << index_bits;
    const u8* weights = WeightTable(index_bits);
    std::array<std::array<u8, 4>, 16> palette{};
    for (u32 i = 0; i < count; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            palette[i][c] = Interpolate(ends[0].value[c], ends[1].value[c], weights[i]);
        }
    }
    for (u32 t = 0; t < 16; ++t) {
        if ((mask >> t & 1) == 0) {
            continue;
        }
        u64 best = ~0ull;
        for (u32 i = 0; i < count; ++i) {
            const u64 error = TexelError(texels.data() + t * 4, palette[i]);
            if (error < best) {
                best = error;
                fit.indices[t] = static_cast<u8>(i);
            }
        }
        fit.error += best;
    }
    return fit;
}

// Least-squares endpoints of the subset's texels for fixed indices.
std::array<std::array<float, 4>, 2> RefitEndpoints(std::span<const u8, 64> texels, u32 mask,
                                                   const std::array<u8, 16>& indices,
                                                   u32 index_bits) {
    const u8* weights = WeightTable(index_bits);
    double aa = 0, ab = 0, bb = 0;
    std::array<double, 4> ax{}, bx{};
    u32 count = 0;
    for (u32 t = 0; t < 16; ++t) {
        if ((mask >> t & 1) == 0) {
            continue;
        }
        ++count;
        const double w = weights[indices[t]] / 64.0;
        const double a = 1.0 - w;
        aa += a * a;
        ab += a * w;
        bb += w * w;
        for (u32 c = 0; c < 4; ++c) {
            ax[c] += a * texels[t * 4 + c];
            bx[c] += w * texels[t * 4 + c];
        }
    }
    std::array<std::array<float, 4>, 2> ends{};
    const double det = aa * bb - ab * ab;
    for (u32 c = 0; c < 4; ++c) {
        if (std::abs(det) < 1e-9) {
            const double mean = (ax[c] + bx[c]) / std::max<double>(count, 1);
            ends[0][c] = ends[1][c] = static_cast<float>(mean);
        } else {
            ends[0][c] = static_cast<float>((bb * ax[c] - ab * bx[c]) / det);
            ends[1][c] = static_cast<float>((aa * bx[c] - ab * ax[c]) / det);
        }
    }
    return ends;
}

// The endpoints along the main axis of the subset's texels: the projections' extremes.
std::array<std::array<float, 4>, 2> AxisEndpoints(std::span<const u8, 64> texels, u32 mask) {
    std::array<double, 4> mean{};
    u32 count = 0;
    for (u32 t = 0; t < 16; ++t) {
        if ((mask >> t & 1) != 0) {
            ++count;
            for (u32 c = 0; c < 4; ++c) {
                mean[c] += texels[t * 4 + c];
            }
        }
    }
    for (double& m : mean) {
        m /= std::max<u32>(count, 1);
    }
    std::array<std::array<double, 4>, 4> cov{};
    for (u32 t = 0; t < 16; ++t) {
        if ((mask >> t & 1) == 0) {
            continue;
        }
        for (u32 i = 0; i < 4; ++i) {
            for (u32 j = 0; j < 4; ++j) {
                cov[i][j] += (texels[t * 4 + i] - mean[i]) * (texels[t * 4 + j] - mean[j]);
            }
        }
    }
    std::array<double, 4> axis{1, 1, 1, 1};
    for (u32 iteration = 0; iteration < 8; ++iteration) {
        std::array<double, 4> next{};
        for (u32 i = 0; i < 4; ++i) {
            for (u32 j = 0; j < 4; ++j) {
                next[i] += cov[i][j] * axis[j];
            }
        }
        const double length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2] +
                                        next[3] * next[3]);
        if (length < 1e-9) {
            break;
        }
        for (u32 i = 0; i < 4; ++i) {
            axis[i] = next[i] / length;
        }
    }
    double low = 1e30, high = -1e30;
    for (u32 t = 0; t < 16; ++t) {
        if ((mask >> t & 1) == 0) {
            continue;
        }
        double projection = 0;
        for (u32 c = 0; c < 4; ++c) {
            projection += (texels[t * 4 + c] - mean[c]) * axis[c];
        }
        low = std::min(low, projection);
        high = std::max(high, projection);
    }
    std::array<std::array<float, 4>, 2> ends{};
    for (u32 c = 0; c < 4; ++c) {
        ends[0][c] = static_cast<float>(mean[c] + axis[c] * low);
        ends[1][c] = static_cast<float>(mean[c] + axis[c] * high);
    }
    return ends;
}

SubsetFit FitSubset(std::span<const u8, 64> texels, u32 mask, u32 bits, u32 index_bits) {
    const auto axis = AxisEndpoints(texels, mask);
    SubsetFit best =
        FitIndices(texels, mask, {Quantize(axis[0], bits), Quantize(axis[1], bits)}, index_bits);
    for (u32 round = 0; round < 2 && best.error != 0; ++round) {
        const auto refit = RefitEndpoints(texels, mask, best.indices, index_bits);
        const SubsetFit next = FitIndices(
            texels, mask, {Quantize(refit[0], bits), Quantize(refit[1], bits)}, index_bits);
        if (next.error >= best.error) {
            break;
        }
        best = next;
    }
    return best;
}

// The anchor texel's index has no top bit: when it would, the endpoints swap.
void FixAnchor(SubsetFit& fit, u32 mask, u32 anchor, u32 index_bits) {
    const u32 top = (1u << index_bits) - 1;
    if (fit.indices[anchor] <= top / 2) {
        return;
    }
    std::swap(fit.ends[0], fit.ends[1]);
    for (u32 t = 0; t < 16; ++t) {
        if ((mask >> t & 1) != 0) {
            fit.indices[t] = static_cast<u8>(top - fit.indices[t]);
        }
    }
}

std::array<u8, 16> PackMode6(const SubsetFit& fit) {
    BitWriter out;
    out.Write(1u << 6, 7);
    for (u32 c = 0; c < 4; ++c) {
        out.Write(fit.ends[0].code[c], 7);
        out.Write(fit.ends[1].code[c], 7);
    }
    out.Write(fit.ends[0].pbit, 1);
    out.Write(fit.ends[1].pbit, 1);
    for (u32 t = 0; t < 16; ++t) {
        out.Write(fit.indices[t], t == 0 ? 3 : 4);
    }
    return out.Bytes();
}

std::array<u8, 16> PackMode7(u32 partition, const std::array<SubsetFit, 2>& fits) {
    BitWriter out;
    out.Write(1u << 7, 8);
    out.Write(partition, 6);
    for (u32 c = 0; c < 4; ++c) {
        for (const SubsetFit& fit : fits) {
            out.Write(fit.ends[0].code[c], 5);
            out.Write(fit.ends[1].code[c], 5);
        }
    }
    for (const SubsetFit& fit : fits) {
        out.Write(fit.ends[0].pbit, 1);
        out.Write(fit.ends[1].pbit, 1);
    }
    for (u32 t = 0; t < 16; ++t) {
        const u32 subset = Partition2[partition] >> (t * 2) & 3;
        const bool anchor = t == 0 || t == Anchor2[partition];
        out.Write(fits[subset].indices[t], anchor ? 1 : 2);
    }
    return out.Bytes();
}

} // namespace

std::optional<Texture> Parse(std::span<const u8> file) {
    if (file.size() < 0x100 || std::memcmp(file.data(), "GNF ", 4) != 0) {
        return std::nullopt;
    }
    u32 contents_size;
    u32 stream_size;
    std::memcpy(&contents_size, file.data() + 4, 4);
    std::memcpy(&stream_size, file.data() + 12, 4);
    if (stream_size != file.size() || file[9] < 1) {
        return std::nullopt;
    }
    Texture texture;
    texture.data_format = Bits(Word(file, 1), 20, 6);
    texture.width = Bits(Word(file, 2), 0, 14) + 1;
    texture.height = Bits(Word(file, 2), 14, 14) + 1;
    const u32 w3 = Word(file, 3);
    texture.swizzle = {Bits(w3, 0, 3), Bits(w3, 3, 3), Bits(w3, 6, 3), Bits(w3, 9, 3)};
    texture.tiling = Bits(w3, 20, 5);
    texture.pitch = Bits(Word(file, 4), 13, 14) + 1;
    texture.data_offset =
        8 + static_cast<u64>(contents_size) + (static_cast<u64>(Word(file, 0)) << 8);
    texture.data_size = Word(file, 7);
    const bool supported =
        (texture.data_format == FormatRgba8 || texture.data_format == FormatBc7) &&
        texture.tiling == TilingThin1D && Bits(w3, 28, 4) == TypeColor2D && Bits(w3, 16, 4) == 0;
    const Layout layout = SurfaceLayout(texture);
    const u64 needed = static_cast<u64>(layout.pitch) * layout.height * layout.element_bytes;
    if (!supported || texture.data_offset + needed > file.size()) {
        return std::nullopt;
    }
    return texture;
}

std::vector<u8> Decode(std::span<const u8> file, const Texture& texture) {
    const Layout layout = SurfaceLayout(texture);
    const u8* data = file.data() + texture.data_offset;
    std::vector<u8> image(static_cast<size_t>(texture.width) * texture.height * 4);
    std::array<u8, 4> surface{};
    std::array<u8, 64> block{};
    u64 decoded = ~0ull;
    for (u32 y = 0; y < texture.height; ++y) {
        for (u32 x = 0; x < texture.width; ++x) {
            const u32 ex = x / layout.block;
            const u32 ey = y / layout.block;
            const u64 index = TiledIndex(layout, ex, ey);
            if (texture.data_format == FormatBc7) {
                if (index != decoded) {
                    block = DecodeBc7(std::span<const u8, 16>(data + index * 16, 16));
                    decoded = index;
                }
                std::memcpy(surface.data(), block.data() + ((y % 4) * 4 + x % 4) * 4, 4);
            } else {
                std::memcpy(surface.data(), data + index * 4, 4);
            }
            u8* out = image.data() + (static_cast<size_t>(y) * texture.width + x) * 4;
            for (u32 c = 0; c < 4; ++c) {
                const u32 select = texture.swizzle[c];
                out[c] = select == 0 ? 0 : select == 1 ? 255 : surface[(select - 4) & 3];
            }
        }
    }
    return image;
}

void Encode(std::span<u8> file, const Texture& texture, std::span<const u8> image) {
    const Layout layout = SurfaceLayout(texture);
    u8* data = file.data() + texture.data_offset;
    const auto unswizzle = [&](const u8* pixel, u8* surface) {
        for (u32 c = 0; c < 4; ++c) {
            const u32 select = texture.swizzle[c];
            if (select >= 4) {
                surface[(select - 4) & 3] = pixel[c];
            }
        }
    };
    if (texture.data_format == FormatRgba8) {
        for (u32 y = 0; y < texture.height; ++y) {
            for (u32 x = 0; x < texture.width; ++x) {
                u8* element = data + TiledIndex(layout, x, y) * 4;
                unswizzle(image.data() + (static_cast<size_t>(y) * texture.width + x) * 4, element);
            }
        }
        return;
    }
    const u32 blocks_x = (texture.width + 3) / 4;
    const u32 blocks_y = (texture.height + 3) / 4;
    for (u32 by = 0; by < blocks_y; ++by) {
        for (u32 bx = 0; bx < blocks_x; ++bx) {
            u8* stored = data + TiledIndex(layout, bx, by) * 16;
            const std::array<u8, 64> old = DecodeBc7(std::span<const u8, 16>(stored, 16));
            std::array<u8, 64> texels = old;
            for (u32 ty = 0; ty < 4; ++ty) {
                for (u32 tx = 0; tx < 4; ++tx) {
                    const u32 x = bx * 4 + tx;
                    const u32 y = by * 4 + ty;
                    if (x < texture.width && y < texture.height) {
                        unswizzle(image.data() + (static_cast<size_t>(y) * texture.width + x) * 4,
                                  texels.data() + (ty * 4 + tx) * 4);
                    }
                }
            }
            if (texels != old) {
                const std::array<u8, 16> encoded = EncodeBc7(texels);
                std::memcpy(stored, encoded.data(), encoded.size());
            }
        }
    }
}

std::array<u8, 64> DecodeBc7(std::span<const u8, 16> block) {
    std::array<u8, 64> texels{};
    u32 mode_index = 0;
    while (mode_index < 8 && (block[0] >> mode_index & 1) == 0) {
        ++mode_index;
    }
    if (mode_index == 8) {
        return texels;
    }
    const Mode& mode = Modes[mode_index];
    BitReader bits(block);
    bits.Read(mode_index + 1);
    const u32 partition = bits.Read(mode.partition_bits);
    const u32 rotation = bits.Read(mode.rotation_bits);
    const u32 selection = bits.Read(mode.selection_bits);
    const u32 endpoints = mode.subsets * 2u;
    std::array<std::array<u32, 4>, 6> raw{};
    for (u32 c = 0; c < 3; ++c) {
        for (u32 e = 0; e < endpoints; ++e) {
            raw[e][c] = bits.Read(mode.color_bits);
        }
    }
    for (u32 e = 0; e < endpoints && mode.alpha_bits != 0; ++e) {
        raw[e][3] = bits.Read(mode.alpha_bits);
    }
    u32 color_bits = mode.color_bits;
    u32 alpha_bits = mode.alpha_bits;
    if (mode.endpoint_pbits != 0 || mode.shared_pbits != 0) {
        std::array<u32, 6> pbits{};
        if (mode.endpoint_pbits != 0) {
            for (u32 e = 0; e < endpoints; ++e) {
                pbits[e] = bits.Read(1);
            }
        } else {
            for (u32 s = 0; s < mode.subsets; ++s) {
                pbits[s * 2] = pbits[s * 2 + 1] = bits.Read(1);
            }
        }
        for (u32 e = 0; e < endpoints; ++e) {
            for (u32 c = 0; c < 4; ++c) {
                raw[e][c] = raw[e][c] << 1 | pbits[e];
            }
        }
        ++color_bits;
        if (alpha_bits != 0) {
            ++alpha_bits;
        }
    }
    std::array<std::array<u8, 4>, 6> ends{};
    for (u32 e = 0; e < endpoints; ++e) {
        for (u32 c = 0; c < 3; ++c) {
            ends[e][c] = Unquantize(raw[e][c], color_bits);
        }
        ends[e][3] = alpha_bits != 0 ? Unquantize(raw[e][3], alpha_bits) : 255;
    }
    std::array<u8, 16> primary{};
    std::array<u8, 16> secondary{};
    for (u32 t = 0; t < 16; ++t) {
        primary[t] =
            static_cast<u8>(bits.Read(mode.index_bits - (IsAnchor(mode, partition, t) ? 1 : 0)));
    }
    for (u32 t = 0; t < 16 && mode.index2_bits != 0; ++t) {
        secondary[t] = static_cast<u8>(bits.Read(mode.index2_bits - (t == 0 ? 1 : 0)));
    }
    for (u32 t = 0; t < 16; ++t) {
        const u32 subset = SubsetOf(mode, partition, t);
        const std::array<u8, 4>& e0 = ends[subset * 2];
        const std::array<u8, 4>& e1 = ends[subset * 2 + 1];
        u8 color_weight;
        u8 alpha_weight;
        if (mode.index2_bits == 0) {
            color_weight = alpha_weight = WeightTable(mode.index_bits)[primary[t]];
        } else if (selection == 0) {
            color_weight = WeightTable(mode.index_bits)[primary[t]];
            alpha_weight = WeightTable(mode.index2_bits)[secondary[t]];
        } else {
            color_weight = WeightTable(mode.index2_bits)[secondary[t]];
            alpha_weight = WeightTable(mode.index_bits)[primary[t]];
        }
        u8* texel = texels.data() + t * 4;
        for (u32 c = 0; c < 3; ++c) {
            texel[c] = Interpolate(e0[c], e1[c], color_weight);
        }
        texel[3] = Interpolate(e0[3], e1[3], alpha_weight);
        if (rotation != 0) {
            std::swap(texel[3], texel[rotation - 1]);
        }
    }
    return texels;
}

std::array<u8, 16> EncodeBc7(std::span<const u8, 64> texels) {
    SubsetFit single = FitSubset(texels, 0xffff, 7, 4);
    FixAnchor(single, 0xffff, 0, 4);
    std::array<u8, 16> best = PackMode6(single);
    u64 best_error = single.error;
    for (u32 partition = 0; partition < 64 && best_error != 0; ++partition) {
        u32 second = 0;
        for (u32 t = 0; t < 16; ++t) {
            second |= (Partition2[partition] >> (t * 2) & 1) << t;
        }
        const u32 first = ~second & 0xffff;
        std::array<SubsetFit, 2> fits{FitSubset(texels, first, 5, 2),
                                      FitSubset(texels, second, 5, 2)};
        const u64 error = fits[0].error + fits[1].error;
        if (error >= best_error) {
            continue;
        }
        FixAnchor(fits[0], first, 0, 2);
        FixAnchor(fits[1], second, Anchor2[partition], 2);
        best = PackMode7(partition, fits);
        best_error = error;
    }
    return best;
}

} // namespace PromptPacks::Gnf
