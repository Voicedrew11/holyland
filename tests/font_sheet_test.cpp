// The game's fonts made sharper (port/gs/shared/font_sheet.h): the sheet enlarged as the GS's bilinear filter reads it,
// the letters drawn in the CLUT's levels where the game has ink and centred on it, nothing done where the CLUT shows no
// set's picture, and a texture that is not one of the disc's sheets left alone.
#include "font_sheet.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "font_letters_data.h"

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t sheet = 512, fine = sheet * font_sheet_scale;
constexpr uint32_t ink = 0x80A5A5A5u, faint = 0x806A6A6Au, darker = 0x80323232u, none = 0x00010101u;

// A CLUT showing the high bits' picture, as font.tmr's: 0-3 ink, 4-7 and 8-11 fainter, 12-15 none
std::array<uint32_t, 256> letters_clut() {
    std::array<uint32_t, 256> clut{};
    const uint32_t by_high[4] = {ink, faint, darker, none};
    for (uint32_t i = 0; i < 16; i++) {
        clut[i] = by_high[i >> 2];
    }
    return clut;
}

uint32_t at(const std::vector<uint32_t> &image, uint32_t x, uint32_t y) {
    return image[size_t(y) * fine + x];
}

// A sheet whose high bits are none everywhere (index 12) but for a block of core (index 0) in the messages' 'H' cell
// (cell 40: texels 400-409, 460-479), at its texels 402-407 across
DecodedTexture test_sheet() {
    DecodedTexture t;
    t.kind = TexelKind::Index8;
    t.width_px = t.height_px = sheet;
    t.levels = 1;
    t.indices.assign(size_t(sheet) * sheet, 12);
    for (uint32_t y = 463; y < 478; y++) {
        for (uint32_t x = 402; x < 408; x++) {
            t.indices[size_t(y) * sheet + x] = 0;
        }
    }
    return t;
}

void sharpened_only_where_the_clut_shows_letters() {
    DecodedTexture t = test_sheet();
    std::vector<uint32_t> colours(size_t(sheet) * sheet, none);
    std::array<uint32_t, 256> flat{};
    flat.fill(none);
    CHECK(sharpened_sheet(t, colours, flat).empty()); // (no picture shown: every index the same colour)
    std::array<uint32_t, 256> mixed = letters_clut();
    mixed[1] = faint; // (the colours follow neither the high bits nor the low)
    CHECK(sharpened_sheet(t, colours, mixed).empty());
    CHECK(sharpened_sheet(t, std::span<const uint32_t>(colours).first(100), letters_clut()).empty());
    CHECK(sharpened_sheet(t, colours, letters_clut()).size() == size_t(fine) * fine);
    DecodedTexture small = t;
    small.width_px = 256;
    CHECK(sharpened_sheet(small, colours, letters_clut()).empty());
}

void enlarged_as_bilinear() {
    // left half 0, right half 0x40 in every channel: across the step at texel 256 the fine texels ramp over one
    // coarse texel
    DecodedTexture t = test_sheet();
    std::vector<uint32_t> colours(size_t(sheet) * sheet);
    for (uint32_t y = 0; y < sheet; y++) {
        for (uint32_t x = 0; x < sheet; x++) {
            colours[size_t(y) * sheet + x] = x < 256 ? 0u : 0x40404040u;
        }
    }
    std::vector<uint32_t> out = sharpened_sheet(t, colours, letters_clut());
    CHECK(!out.empty());
    if (out.empty()) {
        return;
    }
    uint32_t y = 40 * font_sheet_scale; // (a row between the menus' letters and the messages')
    CHECK(at(out, 0, y) == 0);                 // (the left edge clamped)
    CHECK(at(out, 1017, y) == 0);              // (texel 254's last fine texel: 254 and 255 both 0)
    CHECK((at(out, 1022, y) & 0xFF) == 0x08); // (2 * 1022 + 1 - 4 = 2041 = 255 * 8 + 1: 1/8 of the way to 256)
    CHECK((at(out, 1025, y) & 0xFF) == 0x38); // (2051 = 256 * 8 + 3, less 8: 7/8 of the way)
    CHECK(at(out, 1030, y) == 0x40404040u);
    CHECK(at(out, fine - 1, y) == 0x40404040u); // (the right edge clamped)
}

void letters_drawn_where_the_game_has_ink() {
    DecodedTexture t = test_sheet();
    std::vector<uint32_t> colours(size_t(sheet) * sheet, none);
    std::vector<uint32_t> out = sharpened_sheet(t, colours, letters_clut());
    CHECK(!out.empty());
    if (out.empty()) {
        return;
    }
    // 'H': the core's colour on its stems, none between them above the bar, every texel one of the levels' colours or
    // a blend of two (none brighter or more opaque than the core)
    uint32_t x0 = 400 * font_sheet_scale, y0 = 460 * font_sheet_scale;
    uint32_t cores = 0, strays = 0;
    for (uint32_t y = 0; y < font_letters::cell_height; y++) {
        for (uint32_t x = 0; x < font_letters::cell_width; x++) {
            uint32_t c = at(out, x0 + x, y0 + y);
            cores += c == ink;
            strays += (c & 0xFF) > (ink & 0xFF) || (c >> 24) > (ink >> 24); // (nothing past the core)
        }
    }
    CHECK(cores > 100);
    CHECK(strays == 0);
    CHECK(at(out, x0 + 20, y0 + 25) == none); // (between the stems, above the bar)
    // its ink centred where the game's is: the block's middle, texel 405 (fine 1620), is the letter's
    uint32_t left = fine, right = 0;
    for (uint32_t x = 0; x < font_letters::cell_width; x++) {
        for (uint32_t y = 0; y < font_letters::cell_height; y++) {
            if (at(out, x0 + x, y0 + y) == ink) {
                left = std::min(left, x0 + x);
                right = std::max(right, x0 + x);
            }
        }
    }
    CHECK(left + right + 1 >= 2 * 1620 - 4 && left + right + 1 <= 2 * 1620 + 4);
    // a cell the game has no ink in ('G', cell 39) left as the sheet enlarged
    CHECK(at(out, 390 * font_sheet_scale + 20, y0 + 40) == none);
}

// The kana row's punctuation (texel row 180, the low bits): the slash's cell (243: texels 360-379) drawn as a slash,
// the 、 beside the ? (cell 13: texels 260-279) kept as the game has it
void full_width_slash_drawn() {
    DecodedTexture t;
    t.kind = TexelKind::Index8;
    t.width_px = t.height_px = sheet;
    t.levels = 1;
    t.indices.assign(size_t(sheet) * sheet, 3); // (low bits 3: none)
    for (uint32_t y = 183; y < 197; y++) {
        for (uint32_t x = 366; x < 374; x++) {
            t.indices[size_t(y) * sheet + x] = 0;
            t.indices[size_t(y) * sheet + x - 100] = 0;
        }
    }
    std::array<uint32_t, 256> clut{};
    const uint32_t by_low[4] = {ink, faint, darker, none};
    for (uint32_t i = 0; i < 16; i++) {
        clut[i] = by_low[i & 3];
    }
    std::vector<uint32_t> colours(size_t(sheet) * sheet);
    for (size_t i = 0; i < colours.size(); i++) {
        colours[i] = clut[t.indices[i]];
    }
    std::vector<uint32_t> out = sharpened_sheet(t, colours, clut);
    CHECK(!out.empty());
    if (out.empty()) {
        return;
    }
    // (the slash's core: further right near its top than near its foot)
    auto core_middle = [&](uint32_t y) {
        uint32_t sum = 0, n = 0;
        for (uint32_t x = 360 * font_sheet_scale; x < 380 * font_sheet_scale; x++) {
            if (at(out, x, y) == ink) {
                sum += x;
                n++;
            }
        }
        return n ? sum / n : 0;
    };
    uint32_t high = core_middle(186 * font_sheet_scale), low = core_middle(194 * font_sheet_scale);
    CHECK(high > 0 && low > 0 && high > low + 4);
    CHECK(at(out, 270 * font_sheet_scale, 190 * font_sheet_scale) == ink); // (、 kept: the block as it is)
}

void only_the_disc_sheets_known() {
    DecodedTexture t = test_sheet();
    CHECK(!is_font_sheet(t));
    std::vector<uint32_t> colours(size_t(sheet) * sheet, none);
    CHECK(sharper_font_sheet(t, colours, letters_clut()).empty());
    DecodedTexture rgba;
    rgba.width_px = rgba.height_px = sheet;
    rgba.levels = 1;
    CHECK(!is_font_sheet(rgba)); // (Rgba8, no indices)
}

} // namespace

int main() {
    sharpened_only_where_the_clut_shows_letters();
    enlarged_as_bilinear();
    letters_drawn_where_the_game_has_ink();
    full_width_slash_drawn();
    only_the_disc_sheets_known();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("font_sheet: ok\n");
    return 0;
}
