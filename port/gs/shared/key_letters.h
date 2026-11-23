// The letters the menus' buttons are printed with (button_pictures.h), as fonts baked into pictures of their letters by
// tools/make_key_letters.py: the keyboard's keys in Barlow Semi Condensed SemiBold (key_letters_data.h), the Xbox
// buttons' A B X Y in Chakra Petch Bold (xbox_letters_data.h). The arrow keys' arrows (code page 437's characters,
// arrow_up and so on) are shapes of their own, in any font. A character the font has no picture of is left out.
//
// Plain data: any thread.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pcr {

// The arrows' characters in a label (as code page 437 has them)
constexpr char arrow_up = '\x18', arrow_down = '\x19', arrow_right = '\x1a', arrow_left = '\x1b';

// A letter's picture: its place from the pen on the baseline (left; top, rows above it), in rows
struct LetterGlyph {
    char character;
    int16_t left, top;
    uint16_t width, height, advance;
    uint32_t start; // (in its font's coverage)
};

// A baked font: its letters' pictures, cap_height rows to a capital
struct LetterFont {
    uint32_t cap_height;
    std::span<const LetterGlyph> glyphs;
    std::span<const uint8_t> coverage;
};

const LetterFont &key_font();  // (the keyboard's keys)
const LetterFont &xbox_font(); // (the Xbox buttons' letters)

// A line of text in a baked font, laid out in a picture's texels
class LetterText {
public:
    // text centred on (centre_x, centre_y), cap_height texels tall (a capital's height)
    LetterText(const LetterFont &font, std::string_view text, float centre_x, float centre_y, float cap_height);

    // How much of the text's ink is at a point, 0 to 1 (the letters' coverage, read between their rows)
    float ink(float x, float y) const;

    // How wide the text is, in texels, for cap_height texels tall
    static float width_for(const LetterFont &font, std::string_view text, float cap_height);

private:
    struct Placed {
        int glyph = -1;            // (in font_->glyphs; -1: an arrow)
        char arrow = 0;
        float x0 = 0, y0 = 0;      // (its picture's corner, texels)
    };
    const LetterFont *font_;
    std::vector<Placed> placed_;
    float scale_;                  // (texels to a row of the letters' pictures)
    float left_, top_, right_, bottom_;
};

} // namespace pcr
