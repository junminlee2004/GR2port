// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "core/prompt_packs/gnf.h"
#include "core/prompt_packs/keyboard_prompts.h"
#include "core/prompt_packs/keyboard_symbols.h"

namespace PromptPacks::Keyboard {
namespace {

// A button with no key bound.
const Label Dash{{}, "minus"};

// ------------------------------------------------------------------------------------ drawing
using Coverage = std::vector<float>;

struct Canvas {
    u32 width;
    u32 height;
    Coverage* coverage;
};

void Spans(int y, int count, const FT_Span* spans, void* user) {
    const Canvas& canvas = *static_cast<Canvas*>(user);
    const int row = static_cast<int>(canvas.height) - 1 - y;
    if (row < 0 || row >= static_cast<int>(canvas.height)) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        const int end = std::min(spans[i].x + spans[i].len, static_cast<int>(canvas.width));
        for (int x = std::max(static_cast<int>(spans[i].x), 0); x < end; ++x) {
            float& value = (*canvas.coverage)[static_cast<size_t>(row) * canvas.width + x];
            value = std::max(value, spans[i].coverage / 255.0f);
        }
    }
}

class Painter {
public:
    explicit Painter(std::span<const u8> font) {
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
            return;
        }
        if (FT_New_Memory_Face(library, font.data(), static_cast<FT_Long>(font.size()), 0, &face) !=
            0) {
            face = nullptr;
            return;
        }
        const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        cap_units = os2 != nullptr && os2->sCapHeight > 0 ? os2->sCapHeight : 0;
        if (cap_units == 0 &&
            FT_Load_Char(face, 'H', FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ==
                0) {
            FT_BBox box;
            FT_Outline_Get_CBox(&face->glyph->outline, &box);
            cap_units = static_cast<float>(box.yMax);
        }
    }

    ~Painter() {
        if (face != nullptr) {
            FT_Done_Face(face);
        }
        if (library != nullptr) {
            FT_Done_FreeType(library);
        }
    }

    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;

    bool Ready() const {
        return face != nullptr && cap_units > 0;
    }

    // The label's coverage in a width x height image: cap height cap centred at (cx, cy), at most
    // max_width wide when that is above 0.
    Coverage Draw(const Label& label, float cx, float cy, float cap, float max_width, u32 width,
                  u32 height) {
        Coverage coverage(static_cast<size_t>(width) * height, 0.0f);
        if (!label.symbol.empty()) {
            DrawSymbol(label.symbol, cx, cy, cap, max_width, width, height, coverage);
        } else if (!label.text.empty()) {
            DrawText(label.text, cx, cy, cap, max_width, width, height, coverage);
        }
        return coverage;
    }

private:
    void Render(FT_Outline& outline, u32 width, u32 height, Coverage& coverage) {
        Canvas canvas{width, height, &coverage};
        FT_Raster_Params params{};
        params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
        params.gray_spans = &Spans;
        params.user = &canvas;
        params.clip_box = {0, 0, static_cast<FT_Pos>(width), static_cast<FT_Pos>(height)};
        FT_Outline_Render(library, &outline, &params);
    }

    void DrawText(std::string_view text, float cx, float cy, float cap, float max_width, u32 width,
                  u32 height, Coverage& coverage) {
        constexpr FT_Int32 Flags = FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING;
        struct Placed {
            FT_UInt glyph;
            FT_Pos pen;
        };
        std::vector<Placed> placed;
        FT_BBox ink{LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN};
        FT_Pos pen = 0;
        for (const char c : text) {
            const FT_UInt glyph = FT_Get_Char_Index(face, static_cast<unsigned char>(c));
            if (FT_Load_Glyph(face, glyph, Flags) != 0) {
                continue;
            }
            if (face->glyph->outline.n_points > 0) {
                FT_BBox box;
                FT_Outline_Get_CBox(&face->glyph->outline, &box);
                ink.xMin = std::min(ink.xMin, pen + box.xMin);
                ink.xMax = std::max(ink.xMax, pen + box.xMax);
                ink.yMin = std::min(ink.yMin, box.yMin);
                ink.yMax = std::max(ink.yMax, box.yMax);
            }
            placed.push_back({glyph, pen});
            pen += face->glyph->advance.x;
        }
        if (ink.xMin > ink.xMax) {
            return;
        }
        float scale = cap / cap_units;
        float condense = 1.0f;
        const float ink_width = static_cast<float>(ink.xMax - ink.xMin);
        if (max_width > 0 && ink_width * scale > max_width) {
            condense = std::max(0.78f, max_width / (ink_width * scale));
            if (ink_width * scale * condense > max_width) {
                scale = max_width / (ink_width * condense);
            }
        }
        const float middle_x = static_cast<float>(ink.xMin + ink.xMax) / 2.0f;
        // Punctuation sits on its own ink box; names sit on the cap height.
        const bool symbol_like =
            text.size() == 1 && !std::isalnum(static_cast<unsigned char>(text[0]));
        const float middle_y =
            symbol_like ? static_cast<float>(ink.yMin + ink.yMax) / 2.0f : cap_units / 2.0f;
        for (const Placed& glyph : placed) {
            if (FT_Load_Glyph(face, glyph.glyph, Flags) != 0 ||
                face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
                continue;
            }
            FT_Matrix matrix{static_cast<FT_Fixed>(scale * condense * 64.0f * 65536.0f), 0, 0,
                             static_cast<FT_Fixed>(scale * 64.0f * 65536.0f)};
            FT_Outline& outline = face->glyph->outline;
            FT_Outline_Transform(&outline, &matrix);
            const float origin_x = cx + static_cast<float>(glyph.pen - middle_x) * scale * condense;
            const float origin_y = static_cast<float>(height) - cy - middle_y * scale;
            FT_Outline_Translate(&outline, static_cast<FT_Pos>(std::lround(origin_x * 64.0f)),
                                 static_cast<FT_Pos>(std::lround(origin_y * 64.0f)));
            Render(outline, width, height, coverage);
        }
    }

    void DrawSymbol(std::string_view name, float cx, float cy, float cap, float max_width,
                    u32 width, u32 height, Coverage& coverage) {
        const auto symbol = std::ranges::find(Symbols, name, &Symbol::name);
        if (symbol == Symbols.end() || symbol->points.empty()) {
            return;
        }
        s16 x0 = SHRT_MAX, y0 = SHRT_MAX, x1 = SHRT_MIN, y1 = SHRT_MIN;
        for (size_t i = 0; i < symbol->points.size(); i += 2) {
            x0 = std::min(x0, symbol->points[i]);
            x1 = std::max(x1, symbol->points[i]);
            y0 = std::min(y0, symbol->points[i + 1]);
            y1 = std::max(y1, symbol->points[i + 1]);
        }
        float box_width = cap * symbol->width;
        if (max_width > 0) {
            box_width = std::min(box_width, max_width);
        }
        const float scale =
            std::min(cap * symbol->height / std::max(y1 - y0, 1), box_width / std::max(x1 - x0, 1));
        const float middle_x = (x0 + x1) / 2.0f;
        const float middle_y = (y0 + y1) / 2.0f;
        const size_t count = symbol->points.size() / 2;
        std::vector<FT_Vector> points(count);
        std::vector<unsigned char> tags(count, FT_CURVE_TAG_ON);
        std::vector<unsigned short> ends(symbol->contour_ends.size());
        for (size_t i = 0; i < count; ++i) {
            const float x = cx + (symbol->points[i * 2] - middle_x) * scale;
            const float y =
                static_cast<float>(height) - cy + (symbol->points[i * 2 + 1] - middle_y) * scale;
            points[i] = {static_cast<FT_Pos>(std::lround(x * 64.0f)),
                         static_cast<FT_Pos>(std::lround(y * 64.0f))};
        }
        for (size_t i = 0; i < ends.size(); ++i) {
            ends[i] = static_cast<unsigned short>(symbol->contour_ends[i] - 1);
        }
        FT_Outline outline{};
        outline.n_contours = static_cast<decltype(outline.n_contours)>(ends.size());
        outline.n_points = static_cast<decltype(outline.n_points)>(count);
        outline.points = points.data();
        outline.tags = tags.data();
        outline.contours = ends.data();
        outline.flags = FT_OUTLINE_NONE;
        Render(outline, width, height, coverage);
    }

    FT_Library library{};
    FT_Face face{};
    float cap_units{};
};

// ---------------------------------------------------------------------------------- compositing
// Premultiplied RGBA in 0..1.
struct Image {
    u32 width;
    u32 height;
    std::vector<float> texels;

    float* At(u32 x, u32 y) {
        return texels.data() + (static_cast<size_t>(y) * width + x) * 4;
    }
};

Image FromRgba8(std::span<const u8> rgba, u32 width, u32 height) {
    Image image{width, height, std::vector<float>(rgba.size())};
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const float alpha = rgba[i + 3] / 255.0f;
        for (u32 c = 0; c < 3; ++c) {
            image.texels[i + c] = rgba[i + c] / 255.0f * alpha;
        }
        image.texels[i + 3] = alpha;
    }
    return image;
}

std::vector<u8> ToRgba8(const Image& image) {
    std::vector<u8> rgba(image.texels.size());
    for (size_t i = 0; i < rgba.size(); i += 4) {
        const float alpha = std::clamp(image.texels[i + 3], 0.0f, 1.0f);
        for (u32 c = 0; c < 3; ++c) {
            const float value = alpha > 0 ? image.texels[i + c] / alpha : 0.0f;
            rgba[i + c] = static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
        rgba[i + 3] = static_cast<u8>(std::lround(alpha * 255.0f));
    }
    return rgba;
}

// Paints color (0..255) at coverage times opacity over the image.
void Over(Image& image, const Coverage& coverage, const std::array<float, 3>& color,
          float opacity) {
    for (size_t i = 0; i < coverage.size(); ++i) {
        const float a = std::clamp(coverage[i] * opacity, 0.0f, 1.0f);
        if (a <= 0) {
            continue;
        }
        float* texel = image.texels.data() + i * 4;
        for (u32 c = 0; c < 3; ++c) {
            texel[c] = color[c] / 255.0f * a + texel[c] * (1 - a);
        }
        texel[3] = a + texel[3] * (1 - a);
    }
}

// Separable Gaussian blur, zero outside.
Coverage Blur(const Coverage& source, u32 width, u32 height, float sigma) {
    const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 3)));
    std::vector<float> kernel(radius * 2 + 1);
    float sum = 0;
    for (int i = -radius; i <= radius; ++i) {
        kernel[i + radius] = std::exp(-0.5f * (i / sigma) * (i / sigma));
        sum += kernel[i + radius];
    }
    for (float& k : kernel) {
        k /= sum;
    }
    Coverage across(source.size(), 0.0f);
    Coverage result(source.size(), 0.0f);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            float value = 0;
            for (int i = -radius; i <= radius; ++i) {
                const int sx = static_cast<int>(x) + i;
                if (sx >= 0 && sx < static_cast<int>(width)) {
                    value += source[static_cast<size_t>(y) * width + sx] * kernel[i + radius];
                }
            }
            across[static_cast<size_t>(y) * width + x] = value;
        }
    }
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            float value = 0;
            for (int i = -radius; i <= radius; ++i) {
                const int sy = static_cast<int>(y) + i;
                if (sy >= 0 && sy < static_cast<int>(height)) {
                    value += across[static_cast<size_t>(sy) * width + x] * kernel[i + radius];
                }
            }
            result[static_cast<size_t>(y) * width + x] = value;
        }
    }
    return result;
}

std::array<float, 3> Color(const nlohmann::json& value) {
    return {value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
}

// The recipes whose letters an Xbox pad colours.
bool TakesLetterColour(const nlohmann::json& slot) {
    const std::string recipe = slot.value("recipe", "");
    return recipe == "face_key" || recipe == "qte_face" || recipe == "button_disc" ||
           recipe == "rekey_word";
}

// Draws one slot's label into the image in the slot's style.
void DrawSlot(Image& image, const nlohmann::json& slot, const Label& label, const Coverage& cv) {
    const u32 width = image.width;
    const u32 height = image.height;
    const std::string style = slot.value("style", "color");
    if (style == "knockout") {
        for (size_t i = 0; i < cv.size(); ++i) {
            for (u32 c = 0; c < 4; ++c) {
                image.texels[i * 4 + c] *= 1 - std::clamp(cv[i], 0.0f, 1.0f);
            }
        }
    } else if (style == "glow") {
        Coverage glow = Blur(cv, width, height, slot.value("sigma", 1.6f));
        // The glow stays on the opaque body, one texel in from its edge.
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                bool inside = true;
                for (int dy = -1; dy <= 1 && inside; ++dy) {
                    for (int dx = -1; dx <= 1 && inside; ++dx) {
                        const int sx = static_cast<int>(x) + dx;
                        const int sy = static_cast<int>(y) + dy;
                        inside = sx >= 0 && sy >= 0 && sx < static_cast<int>(width) &&
                                 sy < static_cast<int>(height) &&
                                 image.At(sx, sy)[3] > 200.0f / 255.0f;
                    }
                }
                if (!inside) {
                    glow[static_cast<size_t>(y) * width + x] = 0;
                }
            }
        }
        const auto rgb = Color(slot.at("rgb"));
        Over(image, glow, rgb, slot.value("opacity", 0.55f));
        Over(image, cv, rgb, 1.0f);
    } else if (style == "neon") {
        const auto core = Color(slot.at("core"));
        const auto glow = Color(slot.at("glow"));
        const Coverage wide = Blur(cv, width, height, 8.0f);
        const Coverage near = Blur(cv, width, height, 2.0f);
        for (size_t i = 0; i < cv.size(); ++i) {
            const float halo = std::clamp(1.35f * wide[i] + 0.45f * near[i], 0.0f, 1.0f);
            for (u32 c = 0; c < 3; ++c) {
                const float value = glow[c] * halo * (1 - cv[i]) + core[c] * cv[i];
                image.texels[i * 4 + c] = std::clamp(value, 0.0f, 255.0f) / 255.0f;
            }
            image.texels[i * 4 + 3] = 1.0f;
        }
    } else if (style == "ring") {
        const auto& ring = slot.at("ring");
        const float rx = ring.at(0).get<float>();
        const float ry = ring.at(1).get<float>();
        const float radius = ring.at(2).get<float>();
        const Coverage glow = Blur(cv, width, height, 1.5f);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                if (std::hypot(x + 0.5f - rx, y + 0.5f - ry) >= radius - 3.0f) {
                    continue;
                }
                const size_t i = static_cast<size_t>(y) * width + x;
                float* texel = image.At(x, y);
                const float old_alpha = texel[3];
                const float alpha =
                    std::max(std::min(old_alpha + 64.0f / 255.0f * glow[i], 1.0f), cv[i]);
                // The colour stays; only the alpha changes.
                for (u32 c = 0; c < 3; ++c) {
                    texel[c] = old_alpha > 0 ? texel[c] / old_alpha * alpha : texel[c];
                }
                texel[3] = alpha;
            }
        }
    } else if (style == "qte") {
        // The letter face takes a colour per row; R1's tab adds an extrusion to the left in the
        // colours of its side faces.
        const auto row_color = [](const nlohmann::json& table, u32 y) {
            return Color(table.at(std::min<size_t>(y, table.size() - 1)));
        };
        const auto paint = [&](const Coverage& coverage, const nlohmann::json& table) {
            for (u32 y = 0; y < height; ++y) {
                const auto color = row_color(table, y);
                for (u32 x = 0; x < width; ++x) {
                    const float a =
                        std::clamp(coverage[static_cast<size_t>(y) * width + x], 0.0f, 1.0f);
                    float* texel = image.At(x, y);
                    for (u32 c = 0; c < 3; ++c) {
                        texel[c] = color[c] / 255.0f * a + texel[c] * (1 - a);
                    }
                    texel[3] = a + texel[3] * (1 - a);
                }
            }
        };
        if (slot.contains("side_rows") && !slot.at("side_rows").is_null()) {
            Coverage extrusion(cv.size(), 0.0f);
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    float value = 0;
                    for (u32 d = 1; d <= 5 && x + d < width; ++d) {
                        value = std::max(value, cv[static_cast<size_t>(y) * width + x + d]);
                    }
                    extrusion[static_cast<size_t>(y) * width + x] =
                        std::clamp(value - cv[static_cast<size_t>(y) * width + x], 0.0f, 1.0f);
                }
            }
            paint(extrusion, slot.at("side_rows"));
        }
        paint(cv, slot.at("rows"));
    } else if (label.rgb && TakesLetterColour(slot)) {
        const auto& rgb = *label.rgb;
        Over(image, cv, {float(rgb[0]), float(rgb[1]), float(rgb[2])}, 1.0f);
    } else {
        Over(image, cv, Color(slot.value("rgb", nlohmann::json::array({255, 255, 255}))), 1.0f);
    }
}

// Copies the texture at offset in from over the same texture in to. False when to has no texture
// of that size there.
bool CopyTexture(const std::vector<u8>& from, std::vector<u8>& to, u64 offset) {
    if (offset + 16 > from.size() || offset + 16 > to.size()) {
        return false;
    }
    u32 from_size;
    u32 to_size;
    std::memcpy(&from_size, from.data() + offset + 12, sizeof(from_size));
    std::memcpy(&to_size, to.data() + offset + 12, sizeof(to_size));
    if (from_size != to_size || offset + from_size > from.size() || offset + to_size > to.size()) {
        return false;
    }
    std::memcpy(to.data() + offset, from.data() + offset, from_size);
    return true;
}

float NumberOr(const nlohmann::json& slot, const char* key, float fallback) {
    const auto it = slot.find(key);
    return it != slot.end() && it->is_number() ? it->get<float>() : fallback;
}

std::string Lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

void Compose(const std::filesystem::path& slots_json, std::span<const u8> font,
             const std::map<std::string, Label>& labels,
             std::map<std::pair<std::string, std::string>, std::vector<u8>>& members,
             const std::set<std::string>* only,
             const std::map<std::pair<std::string, std::string>, std::vector<u8>>* blank) {
    std::ifstream in{slots_json};
    const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
    if (!root.is_object() || !root.contains("slots") || !root.at("slots").is_array()) {
        LOG_ERROR(Loader, "Prompt packs: the keyboard slots are missing or damaged");
        return;
    }
    Painter painter{font};
    if (!painter.Ready()) {
        LOG_ERROR(Loader, "Prompt packs: the keyboard prompts' font did not load");
        return;
    }
    // Slots grouped by the texture they draw into: member and offset of the texture in it.
    std::map<std::tuple<std::string, std::string, u64>, std::vector<const nlohmann::json*>>
        textures;
    for (const nlohmann::json& slot : root.at("slots")) {
        textures[{Lower(slot.value("archive", "")), Lower(slot.value("member", "")),
                  slot.value("offset", u64{0})}]
            .push_back(&slot);
    }
    u32 drawn = 0;
    u32 drawn_textures = 0;
    for (const auto& [where, slots] : textures) {
        const auto& [archive, path, offset] = where;
        if (only != nullptr && std::ranges::none_of(slots, [&](const nlohmann::json* slot) {
                return only->contains(slot->value("button", ""));
            })) {
            continue;
        }
        const auto member = members.find({archive, path});
        if (only != nullptr) {
            if (blank == nullptr || member == members.end()) {
                continue;
            }
            const auto source = blank->find({archive, path});
            if (source == blank->end()) {
                continue;
            }
            if (!CopyTexture(source->second, member->second, offset)) {
                LOG_WARNING(Loader,
                            "Prompt packs: {} differs from the blank texture; its pad "
                            "prompts stay as they are",
                            path);
                continue;
            }
        }
        if (member == members.end() || offset + 16 > member->second.size()) {
            continue;
        }
        ++drawn_textures;
        u32 stream_size;
        std::memcpy(&stream_size, member->second.data() + offset + 12, sizeof(stream_size));
        if (offset + stream_size > member->second.size()) {
            continue;
        }
        const std::span<u8> file{member->second.data() + offset, stream_size};
        const auto texture = Gnf::Parse(file);
        if (!texture) {
            LOG_ERROR(Loader, "Prompt packs: {} is not a texture the keyboard prompts can draw on",
                      path);
            continue;
        }
        Image image = FromRgba8(Gnf::Decode(file, *texture), texture->width, texture->height);
        for (const nlohmann::json* slot : slots) {
            const std::string button = slot->value("button", "");
            const auto label = labels.find(button);
            const Label& shown = label != labels.end() ? label->second : Dash;
            const auto& center = slot->at("center");
            const Coverage cv =
                painter.Draw(shown, center.at(0).get<float>(), center.at(1).get<float>(),
                             NumberOr(*slot, "cap", 20.0f), NumberOr(*slot, "max_width", 0.0f),
                             texture->width, texture->height);
            DrawSlot(image, *slot, shown, cv);
            ++drawn;
        }
        Gnf::Encode(file, *texture, ToRgba8(image));
    }
    LOG_INFO(Loader, "Prompt packs: drew {} {} prompts in {} textures", drawn,
             only != nullptr ? "pad" : "keyboard", drawn_textures);
}

} // namespace PromptPacks::Keyboard
