// The controller's buttons made sharper (port/gs/shared/button_pictures.h): a button's flat body fitted back as flat,
// its symbol drawn in the table's colour where its shapes are, the disc's edge kept (the picture's background outside
// it, the game's alpha inside), cells the table does not list left as they were, and a picture of another size or
// texels left alone, by finer_pictures.h too.
#include "button_pictures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "button_pictures_data.h"
#include "finer_pictures.h"
#include "key_letters.h"

using namespace pcr;
namespace data = pcr::button_pictures_data;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t k = button_picture_scale;
constexpr size_t sheet = 0; // (the table's sheet of buttons, 128 x 64)
constexpr uint32_t width = 128, height = 64;
constexpr uint32_t body_grey = 60, opaque = 0x80;

uint32_t channel(uint32_t colour, uint32_t c) {
    return (colour >> (8 * c)) & 0xFF;
}

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | g << 8 | b << 16 | a << 24;
}

bool near_rgb(uint32_t colour, uint32_t r, uint32_t g, uint32_t b, int tolerance) {
    return std::abs(int(channel(colour, 0)) - int(r)) <= tolerance &&
           std::abs(int(channel(colour, 1)) - int(g)) <= tolerance &&
           std::abs(int(channel(colour, 2)) - int(b)) <= tolerance;
}

std::span<const data::Button> sheet_buttons() {
    const data::Picture &p = data::pictures[sheet];
    return {data::buttons + p.first_button, p.button_count};
}

const data::Button &button_named(const char *symbol) {
    for (const data::Button &b : sheet_buttons()) {
        if (std::string_view(b.symbol) == symbol) {
            return b;
        }
    }
    std::printf("FAIL: no button %s\n", symbol);
    std::exit(1);
}

// A sheet of flat grey discs, the circle's ring drawn in its colour where texels' middles are on it
std::vector<uint32_t> flat_sheet() {
    std::vector<uint32_t> sheet(size_t(width) * height, 0);
    const data::Button &circle = button_named("circle");
    const data::Part &ring = data::parts[circle.first_part];
    for (const data::Button &b : sheet_buttons()) {
        for (uint32_t y = 0; y < data::cell; y++) {
            for (uint32_t x = 0; x < data::cell; x++) {
                double px = x + 0.5, py = y + 0.5;
                if (std::hypot(px - data::centre, py - data::centre) > data::radius) {
                    continue;
                }
                double d = ring.x[1] - std::hypot(px - ring.x[0], py - ring.y[0]);
                bool on_ring = &b == &circle && d >= 0 && d <= ring.stroke;
                sheet[size_t(b.cell_y + y) * width + b.cell_x + x] =
                    on_ring ? rgba(circle.r, circle.g, circle.b, opaque) : rgba(body_grey, body_grey, body_grey, opaque);
            }
        }
    }
    return sheet;
}

uint32_t fine_at(const std::vector<uint32_t> &fine, const data::Button &b, double x, double y) {
    return fine[size_t(b.cell_y * k + uint32_t(y * k)) * width * k + b.cell_x * k + uint32_t(x * k)];
}

ButtonLooks looks_of(ButtonStyle style) {
    ButtonLooks looks;
    looks.style = style;
    looks.keys = {"F", "Escape", "Space", "Q"};
    return looks;
}

std::vector<uint32_t> redrawn(const std::vector<uint32_t> &colours, ButtonStyle style = ButtonStyle::playstation) {
    std::vector<uint32_t> fine = enlarged_as_read(colours, width, height, k);
    CHECK(draw_listed_buttons(sheet, colours, width, height, fine, looks_of(style)));
    return fine;
}

// Whether any finer texel of a button's cell has (about) a colour
bool cell_has(const std::vector<uint32_t> &fine, const data::Button &b, uint32_t r, uint32_t g, uint32_t bl) {
    for (uint32_t y = 0; y < data::cell * k; y++) {
        for (uint32_t x = 0; x < data::cell * k; x++) {
            if (near_rgb(fine[size_t(b.cell_y * k + y) * width * k + b.cell_x * k + x], r, g, bl, 2)) {
                return true;
            }
        }
    }
    return false;
}

void test_circle_redrawn() {
    CHECK(data::pictures[sheet].width == width && data::pictures[sheet].height == height);
    std::vector<uint32_t> fine = redrawn(flat_sheet());
    const data::Button &circle = button_named("circle");
    const data::Part &ring = data::parts[circle.first_part];
    // (the body, inside the ring and out, fitted back flat)
    uint32_t middle = fine_at(fine, circle, ring.x[0], ring.y[0]);
    CHECK(near_rgb(middle, body_grey, body_grey, body_grey, 4));
    CHECK(channel(middle, 3) == opaque);
    uint32_t rim = fine_at(fine, circle, data::centre, data::centre + data::radius - 1);
    CHECK(near_rgb(rim, body_grey, body_grey, body_grey, 4));
    // (the ring, on its stroke's middle, in the table's colour)
    uint32_t on_ring = fine_at(fine, circle, ring.x[0] + ring.x[1] - ring.stroke / 2, ring.y[0]);
    CHECK(near_rgb(on_ring, circle.r, circle.g, circle.b, 1));
    // (the cell's corner is outside the disc)
    CHECK(channel(fine_at(fine, circle, 0.1, 0.1), 3) == 0);
}

void test_unlisted_cells_enlarged() {
    std::vector<uint32_t> colours = flat_sheet();
    std::vector<uint32_t> fine = redrawn(colours);
    std::vector<uint32_t> enlarged = enlarged_as_read(colours, width, height, k);
    // (the sheet's second row, the d-pad's arms and the shoulder buttons, is not redrawn)
    size_t at = size_t(30 * k) * width * k + 30 * k;
    CHECK(fine.size() == enlarged.size() && fine[at] == enlarged[at]);
}

// The hints' sheet is not quite clear round its buttons: its faint background stays round a redrawn one
void test_faint_background_kept() {
    constexpr size_t hints = 1;
    const data::Picture &p = data::pictures[hints];
    CHECK(p.button_count > 0);
    if (p.button_count == 0) {
        return;
    }
    const data::Button &b = data::buttons[p.first_button];
    std::vector<uint32_t> colours(size_t(p.width) * p.height, rgba(1, 1, 1, 2));
    for (uint32_t y = 0; y < data::cell; y++) {
        for (uint32_t x = 0; x < data::cell; x++) {
            if (std::hypot(x + 0.5 - data::centre, y + 0.5 - data::centre) <= data::radius) {
                colours[size_t(b.cell_y + y) * p.width + b.cell_x + x] = rgba(body_grey, body_grey, body_grey, 255);
            }
        }
    }
    std::vector<uint32_t> fine = enlarged_as_read(colours, p.width, p.height, k);
    CHECK(draw_listed_buttons(hints, colours, p.width, p.height, fine, looks_of(ButtonStyle::playstation)));
    auto at = [&](double x, double y) {
        return fine[size_t(b.cell_y * k + uint32_t(y * k)) * p.width * k + b.cell_x * k + uint32_t(x * k)];
    };
    CHECK(channel(at(0.1, 0.1), 3) == 2);
    CHECK(channel(at(data::centre, data::centre + data::radius - 1), 3) == 255);
    CHECK(near_rgb(at(data::centre, data::centre + data::radius - 1), body_grey, body_grey, body_grey, 4));
}

void test_others_left_alone() {
    std::vector<uint32_t> small(size_t(64) * 64, 0), small_fine(small.size() * k * k, 0);
    CHECK(!draw_listed_buttons(sheet, small, 64, 64, small_fine, looks_of(ButtonStyle::playstation)));
    std::vector<uint32_t> sheet_fine(size_t(width) * k * height * k, 0);
    CHECK(!draw_listed_buttons(sheet, flat_sheet(), width, height, sheet_fine, looks_of(ButtonStyle::disc)));
    DecodedTexture texture;
    texture.kind = TexelKind::Index8;
    texture.width_px = width;
    texture.height_px = height;
    texture.levels = 1;
    texture.indices.assign(size_t(width) * height, 7); // (not the disc's sheet)
    CHECK(may_have_buttons(texture));
    CHECK(!has_buttons(texture));
    std::vector<uint32_t> fine(size_t(width) * k * height * k, 0);
    CHECK(!draw_buttons(texture, flat_sheet(), fine, looks_of(ButtonStyle::playstation)));
    CHECK(std::all_of(fine.begin(), fine.end(), [](uint32_t v) { return v == 0; }));
    FinerAsked asked{true, looks_of(ButtonStyle::xbox)};
    std::vector<uint32_t> clut(256);
    CHECK(finer_picture(texture, flat_sheet(), std::span<const uint32_t, 256>(clut.data(), 256), asked).colours.empty());
    // (nothing asked: no picture may be finer)
    CHECK(!may_be_finer(texture, FinerAsked{}));
    CHECK(may_be_finer(texture, asked));
}

// Xbox: the body kept, the letter in its colour where the PlayStation's symbol was, the symbol gone
void test_xbox_letters() {
    std::vector<uint32_t> fine = redrawn(flat_sheet(), ButtonStyle::xbox);
    const data::Button &circle = button_named("circle");
    const data::Part &ring = data::parts[circle.first_part];
    // (the B's red is somewhere; the ring's colour nowhere)
    CHECK(cell_has(fine, circle, 222, 92, 80));
    CHECK(!cell_has(fine, circle, circle.r, circle.g, circle.b));
    // (on the ring's right edge, where no B is: the body)
    CHECK(near_rgb(fine_at(fine, circle, ring.x[0] + ring.x[1] - ring.stroke / 2, ring.y[0]), body_grey, body_grey,
                   body_grey, 4));
    CHECK(channel(fine_at(fine, circle, 0.1, 0.1), 3) == 0);
    CHECK(cell_has(fine, button_named("cross"), 118, 196, 92));
    CHECK(cell_has(fine, button_named("triangle"), 232, 200, 84));
}

// Whether any finer texel of a button's cell is a light legend (every colour channel at least `light`)
bool cell_has_light(const std::vector<uint32_t> &fine, const data::Button &b, uint32_t light) {
    for (uint32_t y = 0; y < data::cell * k; y++) {
        for (uint32_t x = 0; x < data::cell * k; x++) {
            uint32_t c = fine[size_t(b.cell_y * k + y) * width * k + b.cell_x * k + x];
            if (channel(c, 0) >= light && channel(c, 1) >= light && channel(c, 2) >= light && channel(c, 3) > 0) {
                return true;
            }
        }
    }
    return false;
}

// Keyboard: a black key in the button's place, the bound key's label light on its top
void test_keyboard_keys() {
    std::vector<uint32_t> fine = redrawn(flat_sheet(), ButtonStyle::keyboard);
    for (const char *symbol : {"cross", "circle", "square", "triangle"}) {
        const data::Button &b = button_named(symbol);
        CHECK(cell_has_light(fine, b, 150));
        CHECK(channel(fine_at(fine, b, 0.1, 0.1), 3) == 0);                // (the picture's background round it)
        uint32_t top_corner = fine_at(fine, b, 5.0, 4.5);                   // (the top face, away from the label)
        CHECK(channel(top_corner, 3) == opaque);
        CHECK(channel(top_corner, 0) < 80 && channel(top_corner, 1) < 80 && channel(top_corner, 2) < 80);
    }
    // (an unbound key: the key alone)
    ButtonLooks unbound = looks_of(ButtonStyle::keyboard);
    unbound.keys[0].clear();
    std::vector<uint32_t> colours = flat_sheet(), blank = enlarged_as_read(colours, width, height, k);
    CHECK(draw_listed_buttons(sheet, colours, width, height, blank, unbound));
    CHECK(!cell_has_light(blank, button_named("cross"), 150));
}

// The keys' widths as a real keyboard's, and a key drawn on its own that wide
void test_key_widths() {
    CHECK(key_width_u("F") == 1.0f);
    CHECK(key_width_u("Tab") == 1.5f);
    CHECK(key_width_u("Return") == 2.25f);
    CHECK(key_width_u("Right Shift") == 2.75f);
    CHECK(key_width_u("Space") == 3.0f);
    KeyPicture one = key_picture("F", 2), enter = key_picture("Return", 2);
    CHECK(one.width == data::cell * 2 && one.height == data::cell * 2);
    CHECK(enter.height == one.height && enter.width >= one.width * 2);
    CHECK(one.texels.size() == size_t(one.width) * one.height);
    CHECK(channel(one.texels[0], 3) == 0);                                  // (clear round the key)
    CHECK(channel(one.texels[size_t(one.height / 3) * one.width + one.width / 4], 3) == 255);
}

// The keys' letters: ink where a letter's stroke is, none beside it, wider text for more letters
void test_key_text() {
    const LetterText bar(key_font(), "I", 50.0f, 50.0f, 20.0f);
    CHECK(bar.ink(50.0f, 50.0f) > 0.9f);
    CHECK(bar.ink(50.0f, 75.0f) == 0.0f);
    CHECK(bar.ink(70.0f, 50.0f) == 0.0f);
    const LetterText up(key_font(), std::string(1, arrow_up), 50.0f, 50.0f, 20.0f);
    CHECK(up.ink(50.0f, 50.0f) > 0.9f);
    CHECK(LetterText::width_for(key_font(), "SHIFT", 10.0f) > LetterText::width_for(key_font(), "F", 10.0f) * 3);
    CHECK(LetterText::width_for(key_font(), "", 10.0f) == 0.0f);
}

void test_key_label_text() {
    CHECK(key_label_text("F") == "F");
    CHECK(key_label_text("Escape") == "ESC");
    CHECK(key_label_text("Left Shift") == "SHIFT");
    CHECK(key_label_text("Up") == std::string(1, arrow_up));
    CHECK(key_label_text("Mouse left") == "LMB");
    CHECK(key_label_text("Keypad 5") == "5");
    CHECK(key_label_text("a") == "A");
    CHECK(key_label_text("ScrollLock").size() == 5);
    CHECK(key_label_text("").empty());
}

// The looks' hash: the keys count only for the keyboard's
void test_looks_hash() {
    ButtonLooks a = looks_of(ButtonStyle::xbox), b = a;
    b.keys[0] = "G";
    CHECK(a.hash() == b.hash());
    a.style = b.style = ButtonStyle::keyboard;
    CHECK(a.hash() != b.hash());
    CHECK(looks_of(ButtonStyle::disc).hash() == 0);
    CHECK(looks_of(ButtonStyle::xbox).hash() != looks_of(ButtonStyle::playstation).hash());
}

// The hints as the Item menu draws them (measured in the game): the "Keys" hint shows more of the sheet's rows than
// the "Confirm" one in as many of the screen's, so its square would be 12% wider than tall
void test_hint_drawn_round() {
    const std::array<float, face_button_count> ones = {1, 1, 1, 1};
    std::array<HintPart, hint_part_count> parts{};
    const HintSprite confirm = {28704, 30752, 34144, 34400, 191, 320.5f, 128, 160};
    CHECK(hint_parts(confirm, ones, parts) == 0); // (its cross within 2% of round: drawn as it is)
    const HintSprite keys = {32800, 34848, 34144, 34400, 323, 445.5f, 95, 129};
    CHECK(hint_parts(keys, ones, parts) == hint_part_count);
    const float round_width = float(data::cell) * 2 * 256 / 34; // (screen x for the cell, round)
    CHECK(std::abs(parts[1].x1 - parts[1].x0 - round_width) < 0.01f);
    CHECK(parts[1].u0 == 323 && parts[1].u1 == 323 + float(data::cell)); // (the square's own cell, not a key's)
    CHECK(parts[0].x1 == parts[1].x0 && parts[1].x1 == parts[2].x0);
    // (the words moved left by as much as the button was narrowed)
    const float stretched_width = (keys.x1 - keys.x0) / (keys.u1 - keys.u0) * float(data::cell);
    CHECK(std::abs((keys.x1 - parts[2].x1) - (stretched_width - round_width)) < 0.01f);
}

// The Xbox font has the four letters, each a capital tall, and nothing else
void test_xbox_font() {
    for (const char *letter : {"A", "B", "X", "Y"}) {
        CHECK(LetterText::width_for(xbox_font(), letter, 10.0f) > 5.0f);
    }
    CHECK(LetterText::width_for(xbox_font(), "Q", 10.0f) == 0.0f);
    const LetterText x(xbox_font(), "X", 50.0f, 50.0f, 20.0f);
    CHECK(x.ink(50.0f, 50.0f) > 0.9f);  // (where its strokes cross)
    CHECK(x.ink(50.0f, 42.0f) == 0.0f); // (between its arms)
    CHECK(x.ink(50.0f, 62.0f) == 0.0f); // (above and below the letter)
}

} // namespace

int main() {
    test_circle_redrawn();
    test_unlisted_cells_enlarged();
    test_faint_background_kept();
    test_others_left_alone();
    test_xbox_letters();
    test_keyboard_keys();
    test_key_widths();
    test_key_text();
    test_key_label_text();
    test_looks_hash();
    test_xbox_font();
    test_hint_drawn_round();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("button_pictures: all passed\n");
    return 0;
}
