// SPDX-FileCopyrightText: Copyright 2026 GR2port
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "core/prompt_packs/gnf.h"
#include "core/prompt_packs/photo_prompts.h"

namespace PromptPacks::Photo {

namespace {

// A photo function, the binding the packs draw for it, and its icons: the control list's, and a
// small one where it has one. Rotate Camera's button is held with the right stick, so its icon
// joins the two; no pack draws it. It takes the SHARE icon, which the game shows nowhere.
struct Function {
    std::string_view name;
    std::string_view drawn;
    std::string_view icon;
    std::string_view small_icon;
    std::string_view with;
};
constexpr std::array<Function, 4> Functions{{
    {"gr2_hide_ui_pad", "l3,r2", "gcd/photomode_key_19.gnf", "", ""},
    {"gr2_collection_pad", "l3,l1", "gcd/photomode_key_21.gnf", "gcd/photomode_key_21_b.gnf", ""},
    {"gr2_picture_roll_pad", "l3,r1", "gcd/photomode_key_22.gnf", "gcd/photomode_key_22_b.gnf", ""},
    {"gr2_rotate_camera_pad", "", "gcd/photomode_key_23.gnf", "", "rstick"},
}};

// The photo mode's icon of each pad button, by the input config's name, and of the right stick.
// L3 has none of its own: the packs' Hide UI icon begins with it.
constexpr std::array<std::pair<std::string_view, std::string_view>, 16> ButtonIcons{{
    {"circle", "gcd/photomode_key_01.gnf"},
    {"cross", "gcd/photomode_key_02.gnf"},
    {"square", "gcd/photomode_key_03.gnf"},
    {"triangle", "gcd/photomode_key_04.gnf"},
    {"r1", "gcd/photomode_key_05.gnf"},
    {"r2", "gcd/photomode_key_06.gnf"},
    {"l1", "gcd/photomode_key_07.gnf"},
    {"l2", "gcd/photomode_key_08.gnf"},
    {"pad_up", "gcd/photomode_key_09.gnf"},
    {"pad_down", "gcd/photomode_key_10.gnf"},
    {"pad_left", "gcd/photomode_key_11.gnf"},
    {"pad_right", "gcd/photomode_key_12.gnf"},
    {"back", "gcd/photomode_key_13.gnf"},
    {"options", "gcd/photomode_key_18.gnf"},
    {"r3", "gcd/photomode_key_20.gnf"},
    {"rstick", "gcd/photomode_key_17.gnf"},
}};

// Every pack's Hide UI icon: L3 in columns 18 to 61, the plus in 63 to 78 and R2 to column 125,
// centred on row 68, L3 and R2 at 0.585 of their own icons' size with a texel between each part.
constexpr std::string_view JoinedIcon = "gcd/photomode_key_19.gnf";
constexpr u32 L3First = 18;
constexpr u32 L3Last = 61;
constexpr u32 PlusFirst = 63;
constexpr u32 PlusLast = 78;
constexpr u32 JoinedLast = 125;
constexpr float JoinedRow = 68.0f;
constexpr float JoinScale = 0.585f;
constexpr float JoinGap = 1.0f;
constexpr float JoinedWidth = JoinedLast - L3First + 1;

// The back paddles' names on each layout's pad, as the launcher names them: upper left, upper
// right, lower left, lower right.
struct PaddleNames {
    std::string_view layout;
    std::array<std::string_view, 4> names;
};
constexpr std::array<PaddleNames, 5> PaddleNamesOf{{
    {"xbox", {"P3", "P1", "P4", "P2"}},
    {"switch", {"GL", "GR", "SR", "SL"}},
    {"steamdeck", {"L4", "R4", "L5", "R5"}},
    {"steamcontroller", {"L4", "R4", "L5", "R5"}},
    {"playstation", {"LB", "RB", "L Fn", "R Fn"}},
}};

// Premultiplied RGBA in 0..1.
struct Image {
    u32 width{};
    u32 height{};
    std::vector<float> texels;
};

Image FromRgba8(const std::vector<u8>& rgba, u32 width, u32 height) {
    Image image{width, height, std::vector<float>(static_cast<size_t>(width) * height * 4)};
    for (size_t i = 0; i < image.texels.size() && i + 3 < rgba.size(); i += 4) {
        const float alpha = rgba[i + 3] / 255.0f;
        for (size_t c = 0; c < 3; ++c) {
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
        for (size_t c = 0; c < 3; ++c) {
            const float value = alpha > 0 ? image.texels[i + c] / alpha : 0.0f;
            rgba[i + c] = static_cast<u8>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
        rgba[i + 3] = static_cast<u8>(std::lround(alpha * 255.0f));
    }
    return rgba;
}

// The texels of image between columns first and last that hold any alpha, cut out; none when
// there are none.
std::optional<Image> Trim(const Image& image, u32 first = 0, u32 last = 0xffffffff) {
    last = std::min(last, image.width - 1);
    u32 x0 = image.width, y0 = image.height, x1 = 0, y1 = 0;
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = first; x <= last; ++x) {
            if (image.texels[(static_cast<size_t>(y) * image.width + x) * 4 + 3] > 0) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x0 > x1 || y0 > y1) {
        return std::nullopt;
    }
    Image cut{x1 - x0 + 1, y1 - y0 + 1, {}};
    cut.texels.resize(static_cast<size_t>(cut.width) * cut.height * 4);
    for (u32 y = 0; y < cut.height; ++y) {
        const float* from =
            image.texels.data() + (static_cast<size_t>(y0 + y) * image.width + x0) * 4;
        std::copy(from, from + static_cast<size_t>(cut.width) * 4,
                  cut.texels.data() + static_cast<size_t>(y) * cut.width * 4);
    }
    return cut;
}

// Each texel of an axis of to texels, as the texels of the axis of from texels it covers and how
// much of each.
std::vector<std::vector<std::pair<u32, float>>> Footprints(u32 from, u32 to) {
    std::vector<std::vector<std::pair<u32, float>>> footprints(to);
    const float step = static_cast<float>(from) / static_cast<float>(to);
    for (u32 i = 0; i < to; ++i) {
        const float begin = i * step;
        const float end = (i + 1) * step;
        for (u32 j = static_cast<u32>(begin); j < from && static_cast<float>(j) < end; ++j) {
            const float covered = std::min(end, static_cast<float>(j) + 1.0f) -
                                  std::max(begin, static_cast<float>(j));
            if (covered > 0) {
                footprints[i].emplace_back(j, covered / step);
            }
        }
    }
    return footprints;
}

// image at width x height, each texel the average of the texels it covers.
Image Resize(const Image& image, u32 width, u32 height) {
    const auto columns = Footprints(image.width, width);
    const auto rows = Footprints(image.height, height);
    Image wide{width, image.height,
               std::vector<float>(static_cast<size_t>(width) * image.height * 4)};
    for (u32 y = 0; y < image.height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            float* to = wide.texels.data() + (static_cast<size_t>(y) * width + x) * 4;
            for (const auto& [from, share] : columns[x]) {
                const float* texel =
                    image.texels.data() + (static_cast<size_t>(y) * image.width + from) * 4;
                for (size_t c = 0; c < 4; ++c) {
                    to[c] += texel[c] * share;
                }
            }
        }
    }
    Image sized{width, height, std::vector<float>(static_cast<size_t>(width) * height * 4)};
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            float* to = sized.texels.data() + (static_cast<size_t>(y) * width + x) * 4;
            for (const auto& [from, share] : rows[y]) {
                const float* texel =
                    wide.texels.data() + (static_cast<size_t>(from) * width + x) * 4;
                for (size_t c = 0; c < 4; ++c) {
                    to[c] += texel[c] * share;
                }
            }
        }
    }
    return sized;
}

// Draws part over canvas with its top left texel at x, y.
void Over(Image& canvas, const Image& part, int x, int y) {
    for (u32 py = 0; py < part.height; ++py) {
        const int cy = y + static_cast<int>(py);
        if (cy < 0 || cy >= static_cast<int>(canvas.height)) {
            continue;
        }
        for (u32 px = 0; px < part.width; ++px) {
            const int cx = x + static_cast<int>(px);
            if (cx < 0 || cx >= static_cast<int>(canvas.width)) {
                continue;
            }
            const float* from =
                part.texels.data() + (static_cast<size_t>(py) * part.width + px) * 4;
            float* to = canvas.texels.data() +
                        (static_cast<size_t>(cy) * canvas.width + static_cast<u32>(cx)) * 4;
            for (size_t c = 0; c < 4; ++c) {
                to[c] = from[c] + to[c] * (1 - from[3]);
            }
        }
    }
}

std::optional<Image> Icon(const Read& read, std::string_view path) {
    const auto bytes = read(std::string(path));
    if (!bytes) {
        return std::nullopt;
    }
    const auto texture = Gnf::Parse(*bytes);
    if (!texture) {
        return std::nullopt;
    }
    return FromRgba8(Gnf::Decode(*bytes, *texture), texture->width, texture->height);
}

// A part of a binding's icon: its texels, at scale of the button's own icon.
struct Part {
    Image image;
    float scale = 1.0f;
};

std::string_view PaddleName(std::string_view layout, std::string_view paddle) {
    const auto names = std::ranges::find(PaddleNamesOf, layout, &PaddleNames::layout);
    const auto paddles = Input::PadBindings::Paddles();
    const auto at = std::ranges::find(paddles, paddle);
    if (names == PaddleNamesOf.end() || at == paddles.end()) {
        return {};
    }
    return names->names[at - paddles.begin()];
}

std::optional<Part> ButtonPart(std::string_view layout, std::string_view button, const Read& read,
                               const ShoulderKey& shoulder_key,
                               const std::optional<Image>& joined) {
    if (button == "l3") {
        if (!joined) {
            return std::nullopt;
        }
        auto l3 = Trim(*joined, L3First, L3Last);
        return l3 ? std::optional<Part>{Part{std::move(*l3), JoinScale}} : std::nullopt;
    }
    if (Input::PadBindings::IsPaddle(button)) {
        const auto rgba = shoulder_key(std::string(PaddleName(layout, button)));
        if (!rgba) {
            return std::nullopt;
        }
        auto key = Trim(FromRgba8(*rgba, 128, 128));
        return key ? std::optional<Part>{Part{std::move(*key), 1.0f}} : std::nullopt;
    }
    const auto icon = std::ranges::find(ButtonIcons, button,
                                        &std::pair<std::string_view, std::string_view>::first);
    if (icon == ButtonIcons.end()) {
        return std::nullopt;
    }
    const auto image = Icon(read, icon->second);
    auto part = image ? Trim(*image) : std::nullopt;
    return part ? std::optional<Part>{Part{std::move(*part), 1.0f}} : std::nullopt;
}

// The icon of a binding: its buttons joined by the plus as the packs join them, at their scale or
// smaller where the buttons would not fit; a back paddle alone as its shoulder key; nothing for
// none.
std::optional<Image> BindingIcon(std::string_view layout, std::string_view binding,
                                 const Read& read, const ShoulderKey& shoulder_key,
                                 const std::optional<Image>& joined, u32 width, u32 height) {
    Image canvas{width, height, std::vector<float>(static_cast<size_t>(width) * height * 4)};
    if (binding.empty()) {
        return canvas;
    }
    const auto comma = binding.find(',');
    if (comma == std::string_view::npos) {
        const auto rgba = shoulder_key(std::string(PaddleName(layout, binding)));
        return rgba ? std::optional<Image>{FromRgba8(*rgba, 128, 128)} : std::nullopt;
    }
    std::vector<Part> parts;
    for (const std::string_view button : {binding.substr(0, comma), binding.substr(comma + 1)}) {
        auto part = ButtonPart(layout, button, read, shoulder_key, joined);
        if (!part) {
            return std::nullopt;
        }
        parts.push_back(std::move(*part));
    }
    auto plus_image = joined ? Trim(*joined, PlusFirst, PlusLast) : std::nullopt;
    if (!plus_image) {
        return std::nullopt;
    }
    const Part plus{std::move(*plus_image), JoinScale};
    const auto size = [](const Part& part, float scale) {
        return std::pair{std::max(1.0f, std::round(part.image.width * scale / part.scale)),
                         std::max(1.0f, std::round(part.image.height * scale / part.scale))};
    };
    const auto span = [&](float scale) {
        return size(parts[0], scale).first + size(plus, scale).first + size(parts[1], scale).first +
               2 * JoinGap;
    };
    float scale = JoinScale;
    if (span(scale) > JoinedWidth) {
        scale *= JoinedWidth / span(scale);
    }
    float x = JoinedLast + 1 - span(scale);
    for (const Part* part : std::array<const Part*, 3>{&parts[0], &plus, &parts[1]}) {
        const auto [w, h] = size(*part, scale);
        Over(canvas, Resize(part->image, static_cast<u32>(w), static_cast<u32>(h)),
             static_cast<int>(std::lround(x)), static_cast<int>(std::lround(JoinedRow - h / 2)));
        x += w + JoinGap;
    }
    return canvas;
}

// Whether layout's art shows another binding than binding. The PlayStation prompts keep the
// game's touch pad icons of Collection and Picture Roll, which the pad's touch pad opens, unless
// a binding of another button replaces them.
bool Differs(std::string_view layout, const Function& function,
             const Input::PadBindings::Map& photo) {
    if (!function.with.empty()) {
        return true;
    }
    const auto bound = photo.find(function.name);
    const std::string_view binding = bound != photo.end() ? bound->second : function.drawn;
    if (layout == "playstation" && function.name != Functions[0].name) {
        return !binding.empty() && binding != function.drawn;
    }
    return binding != function.drawn;
}

} // namespace

std::vector<std::string> Members() {
    std::vector<std::string> members{std::string(JoinedIcon)};
    for (const auto& [button, icon] : ButtonIcons) {
        members.emplace_back(icon);
    }
    for (const Function& function : Functions) {
        for (const std::string_view icon : {function.icon, function.small_icon}) {
            if (!icon.empty() && std::ranges::find(members, icon) == members.end()) {
                members.emplace_back(icon);
            }
        }
    }
    return members;
}

bool Changed(std::string_view layout, const Input::PadBindings::Map& photo) {
    return std::ranges::any_of(
        Functions, [&](const Function& function) { return Differs(layout, function, photo); });
}

bool UsesPaddle(const Input::PadBindings::Map& photo) {
    return std::ranges::any_of(photo, [](const auto& binding) {
        const std::string_view inputs = binding.second;
        const auto comma = inputs.find(',');
        return Input::PadBindings::IsPaddle(inputs.substr(0, comma)) ||
               (comma != std::string_view::npos &&
                Input::PadBindings::IsPaddle(inputs.substr(comma + 1)));
    });
}

std::map<std::string, std::vector<u8>> Compose(std::string_view layout,
                                               const Input::PadBindings::Map& photo,
                                               const Read& read, const ShoulderKey& shoulder_key) {
    std::map<std::string, std::vector<u8>> drawn;
    const std::optional<Image> joined = Icon(read, JoinedIcon);
    for (const Function& function : Functions) {
        if (!Differs(layout, function, photo)) {
            continue;
        }
        const auto bound = photo.find(function.name);
        std::string binding{bound != photo.end() ? bound->second : function.drawn};
        if (!binding.empty() && !function.with.empty()) {
            binding += "," + std::string(function.with);
        }
        const auto icon = BindingIcon(layout, binding, read, shoulder_key, joined, 128, 128);
        if (!icon) {
            continue;
        }
        for (const std::string_view path : {function.icon, function.small_icon}) {
            if (path.empty()) {
                continue;
            }
            auto bytes = read(std::string(path));
            const auto texture = bytes ? Gnf::Parse(*bytes) : std::nullopt;
            if (!texture) {
                continue;
            }
            const Image sized = texture->width == icon->width && texture->height == icon->height
                                    ? *icon
                                    : Resize(*icon, texture->width, texture->height);
            Gnf::Encode(*bytes, *texture, ToRgba8(sized));
            drawn.emplace(path, std::move(*bytes));
        }
    }
    return drawn;
}

} // namespace PromptPacks::Photo
