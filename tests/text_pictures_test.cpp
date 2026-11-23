// The game's word pictures made sharper (port/gs/shared/text_pictures.h): the old word painted over with the glow
// beside it (or the box's own colour), the word drawn again in the old one's colour on the game's baseline, and a
// texture that is not one of the disc's pictures left alone. Also finer_pictures.h's enlargement, which both kinds use.
#include "text_pictures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "finer_pictures.h"
#include "text_pictures_data.h"

using namespace pcr;
namespace data = pcr::text_pictures_data;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t k = text_picture_scale;
constexpr size_t button_texels = size_t(256) * 64;
constexpr size_t fine_width = size_t(256) * k;
constexpr uint32_t white = 0xFFFFFFFFu, black = 0xFF000000u;

uint32_t channel(uint32_t colour, uint32_t c) {
    return (colour >> (8 * c)) & 0xFF;
}

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | g << 8 | b << 16 | a << 24;
}

bool near(uint32_t a, uint32_t b, uint32_t tolerance) {
    for (uint32_t ch = 0; ch < 4; ch++) {
        if (uint32_t(std::abs(int32_t(channel(a, ch)) - int32_t(channel(b, ch)))) > tolerance) {
            return false;
        }
    }
    return true;
}

// The table's picture whose (first) word is `text`
size_t picture_of(std::string_view text) {
    for (size_t i = 0; i < std::size(data::pictures); i++) {
        if (data::words[data::pictures[i].first_word].text == text) {
            return i;
        }
    }
    std::printf("FAIL: no picture of %.*s\n", int(text.size()), text.data());
    std::exit(1);
}

// A blue glow, a quadratic across times a row's share: what paint_glow fits exactly
uint32_t glow(uint32_t x, uint32_t y) {
    double across = std::max(0.0, 1.0 - (double(x) - 128) * (double(x) - 128) / (130.0 * 130.0));
    double down = std::max(0.0, 1.0 - std::abs(double(y) - 32) / 20.0);
    double v = across * down;
    return rgba(uint32_t(100 * v), uint32_t(150 * v), uint32_t(220 * v), uint32_t(200 * v));
}

// A 256 x 64 picture: `ground` everywhere but a block of white ink at texels x0..x1-1 across, rows 22-41 (a word's
// capitals on the menu's baseline, row 42)
std::vector<uint32_t> button(uint32_t (*ground)(uint32_t, uint32_t), uint32_t x0, uint32_t x1) {
    std::vector<uint32_t> colours(button_texels);
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 256; x++) {
            colours[size_t(y) * 256 + x] = (y >= 22 && y < 42 && x >= x0 && x < x1) ? white : ground(x, y);
        }
    }
    return colours;
}

uint32_t black_ground(uint32_t, uint32_t) {
    return black;
}

// The finer columns (left, right past) where any finer texel in rows y0..y1-1 is near white
std::pair<uint32_t, uint32_t> ink_columns(const std::vector<uint32_t> &fine, uint32_t width, uint32_t y0,
                                          uint32_t y1) {
    uint32_t left = UINT32_MAX, right = 0;
    for (uint32_t y = y0; y < y1; y++) {
        for (uint32_t x = 0; x < width; x++) {
            if (near(fine[size_t(y) * width + x], white, 8)) {
                left = std::min(left, x);
                right = std::max(right, x + 1);
            }
        }
    }
    return {left, right};
}

void enlarged_as_bilinear() {
    // (two texels across: the finer texels between move from one to the other, the outer ones clamped; row 0 of 4)
    std::vector<uint32_t> two = {rgba(0, 0, 0, 0), rgba(200, 100, 40, 255)};
    std::vector<uint32_t> fine = enlarged_as_read(two, 2, 1, 4);
    CHECK(fine.size() == size_t(8) * 4);
    CHECK(fine[0] == two[0] && fine[1] == two[0]);
    CHECK(fine[6] == two[1] && fine[7] == two[1]);
    CHECK(channel(fine[3], 0) > 0 && channel(fine[3], 0) < channel(fine[4], 0) && channel(fine[4], 0) < 200);
    std::vector<uint32_t> flat(size_t(16) * 8, rgba(10, 20, 30, 40));
    for (uint32_t c : enlarged_as_read(flat, 16, 8, k)) {
        CHECK(c == flat[0]);
    }
}

// A wide block of ink on a glow: "Load" drawn again in its middle, the glow put back where the block was beside it
void glow_painted_under_the_word() {
    size_t p = picture_of("Load");
    CHECK(data::pictures[p].background == data::Background::Glow);
    std::vector<uint32_t> colours = button(glow, 30, 226);
    std::vector<uint32_t> fine = redrawn_text_picture(p, colours, 256, 64);
    CHECK(fine.size() == fine_width * 64 * k);
    // (inside the old block, left of the new word: the glow again)
    for (uint32_t x = 36; x < 70; x += 3) {
        for (uint32_t y = 24; y < 40; y += 3) {
            uint32_t fx = x * k + 1, fy = y * k + 1;
            CHECK(near(fine[fy * fine_width + fx], glow(x, y), 4));
        }
    }
    // (the new word white, centred on the block, its capitals on the game's rows; nothing white under the baseline)
    auto [left, right] = ink_columns(fine, 256 * k, 22 * k, 42 * k);
    CHECK(left < right);
    CHECK(std::abs(int32_t(left + right) / 2 - int32_t(128 * k)) <= int32_t(k));
    CHECK(right - left < (226 - 30) * k / 2); // ("Load" is narrower than the block)
    CHECK(ink_columns(fine, 256 * k, 43 * k, 64 * k).first == UINT32_MAX);
    CHECK(ink_columns(fine, 256 * k, 0, 21 * k).first == UINT32_MAX);
}

// White capitals on black: the old ones painted black, the new drawn centred on them
void plain_painted_under_the_word() {
    size_t p = picture_of("NEW GAME");
    CHECK(data::pictures[p].background == data::Background::Plain);
    std::vector<uint32_t> colours(button_texels, black);
    for (uint32_t y = 26; y < 43; y++) {
        for (uint32_t x = 20; x < 236; x++) {
            colours[size_t(y) * 256 + x] = white;
        }
    }
    std::vector<uint32_t> fine = redrawn_text_picture(p, colours, 256, 64);
    auto [left, right] = ink_columns(fine, 256 * k, 26 * k, 43 * k);
    CHECK(left > 20 * k && right < 236 * k);
    CHECK(std::abs(int32_t(left + right) / 2 - int32_t(128 * k)) <= int32_t(k));
    // (beside the new word, where the old block was: black)
    CHECK(fine[fine_width * 34 * k + size_t(22) * k] == black);
    CHECK(fine[fine_width * 34 * k + size_t(233) * k] == black);
}

// A region without ink: nothing painted, nothing drawn
void no_ink_nothing_drawn() {
    size_t p = picture_of("Load");
    std::vector<uint32_t> colours = button(glow, 0, 0);
    std::vector<uint32_t> fine = redrawn_text_picture(p, colours, 256, 64);
    CHECK(fine == enlarged_as_read(colours, 256, 64, k));
}

void only_the_disc_pictures_known() {
    DecodedTexture t;
    t.kind = TexelKind::Index8;
    t.width_px = 256;
    t.height_px = 64;
    t.levels = 1;
    t.indices.assign(button_texels, 3);
    CHECK(may_be_text_picture(t));
    std::vector<uint32_t> colours = button(black_ground, 100, 150);
    CHECK(sharper_text_picture(t, colours).empty());
    FinerPicture none = finer_picture(t, colours, std::array<uint32_t, 256>{});
    CHECK(none.colours.empty() && none.scale == 1);
    t.width_px = 200; // (no picture is 200 wide)
    CHECK(!may_be_text_picture(t));
    DecodedTexture direct;
    direct.width_px = 256;
    direct.height_px = 64;
    direct.levels = 1;
    CHECK(!may_be_text_picture(direct)); // (Rgba8, no indices)
    CHECK(redrawn_text_picture(picture_of("Load"), colours, 128, 64).empty()); // (not the picture's size)
}

} // namespace

int main() {
    enlarged_as_bilinear();
    glow_painted_under_the_word();
    plain_painted_under_the_word();
    no_ink_nothing_drawn();
    only_the_disc_pictures_known();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("text_pictures: ok\n");
    return 0;
}
