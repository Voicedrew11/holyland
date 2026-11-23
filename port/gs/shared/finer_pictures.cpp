// The game's pictures made finer (finer_pictures.h).
#include "finer_pictures.h"

#include <algorithm>
#include <array>

#include "font_sheet.h"
#include "text_pictures.h"

namespace pcr {
namespace {

std::vector<uint32_t> font_sheet_colours(const DecodedTexture &texture, std::span<const uint32_t> colours,
                                         std::span<const uint32_t, 256> clut) {
    return sharper_font_sheet(texture, colours, clut);
}

std::vector<uint32_t> text_picture_colours(const DecodedTexture &texture, std::span<const uint32_t> colours,
                                           std::span<const uint32_t, 256>) {
    return sharper_text_picture(texture, colours);
}

struct Kind {
    bool (*may_be)(const DecodedTexture &);
    std::vector<uint32_t> (*make)(const DecodedTexture &, std::span<const uint32_t>, std::span<const uint32_t, 256>);
    uint32_t scale;
};

bool may_be_font_sheet(const DecodedTexture &texture) {
    return texture.kind == TexelKind::Index8 && texture.width_px == 512 && texture.height_px == 512 &&
           texture.levels == 1;
}

constexpr std::array<Kind, 2> kinds = {{
    {may_be_font_sheet, font_sheet_colours, font_sheet_scale},
    {may_be_text_picture, text_picture_colours, text_picture_scale},
}};

uint32_t channel(uint32_t colour, uint32_t c) {
    return (colour >> (8 * c)) & 0xFF;
}

} // namespace

bool may_be_finer(const DecodedTexture &texture) {
    return std::any_of(kinds.begin(), kinds.end(), [&](const Kind &k) { return k.may_be(texture); });
}

FinerPicture finer_picture(const DecodedTexture &texture, std::span<const uint32_t> colours,
                           std::span<const uint32_t, 256> clut) {
    for (const Kind &k : kinds) {
        if (!k.may_be(texture)) {
            continue;
        }
        std::vector<uint32_t> made = k.make(texture, colours, clut);
        if (!made.empty()) {
            return {std::move(made), k.scale};
        }
    }
    return {};
}

std::vector<uint32_t> enlarged_as_read(std::span<const uint32_t> colours, uint32_t width, uint32_t height,
                                       uint32_t scale) {
    const int32_t k = int32_t(scale);
    const uint32_t fine_w = width * scale, fine_h = height * scale;
    std::vector<uint32_t> out(size_t(fine_w) * fine_h);
    auto clamped = [](int32_t v, uint32_t size) { return uint32_t(std::clamp(v, 0, int32_t(size) - 1)); };
    for (uint32_t fy = 0; fy < fine_h; fy++) {
        // (the coarse place of the fine texel's middle, less half a texel, in units of 1 / (2k))
        int32_t py = int32_t(2 * fy + 1) - k;
        int32_t y0 = py < 0 ? -1 : py / (2 * k);
        uint32_t wy = uint32_t(py - y0 * 2 * k); // (0 to 2k - 1)
        uint32_t ya = clamped(y0, height), yb = clamped(y0 + 1, height);
        for (uint32_t fx = 0; fx < fine_w; fx++) {
            int32_t px = int32_t(2 * fx + 1) - k;
            int32_t x0 = px < 0 ? -1 : px / (2 * k);
            uint32_t wx = uint32_t(px - x0 * 2 * k);
            uint32_t xa = clamped(x0, width), xb = clamped(x0 + 1, width);
            uint32_t a = colours[size_t(ya) * width + xa], b = colours[size_t(ya) * width + xb];
            uint32_t c = colours[size_t(yb) * width + xa], d = colours[size_t(yb) * width + xb];
            uint32_t texel = 0;
            for (uint32_t ch = 0; ch < 4; ch++) {
                uint32_t top = channel(a, ch) * (2 * scale - wx) + channel(b, ch) * wx;
                uint32_t bottom = channel(c, ch) * (2 * scale - wx) + channel(d, ch) * wx;
                uint32_t v = (top * (2 * scale - wy) + bottom * wy + 2 * scale * scale) / (4 * scale * scale);
                texel |= std::min(v, 255u) << (8 * ch);
            }
            out[size_t(fy) * fine_w + fx] = texel;
        }
    }
    return out;
}

} // namespace pcr
