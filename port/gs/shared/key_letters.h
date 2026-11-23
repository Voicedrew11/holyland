// A key's label as printed on the keyboard's keys (button_pictures.h, the Keyboard style): a line of capitals and
// digits in Barlow Semi Condensed SemiBold (key_letters_data.h, made by tools/make_key_letters.py), and the arrow keys'
// arrows (code page 437's characters, stroke_letters.h's arrow_up and so on) as shapes of their own. A character the
// font has no picture of is left out.
//
// Plain data: any thread.
#pragma once

#include <string_view>
#include <vector>

namespace pcr {

class KeyText {
public:
    // text centred on (centre_x, centre_y), cap_height texels tall (a capital's height), in a picture's texels
    KeyText(std::string_view text, float centre_x, float centre_y, float cap_height);

    // How much of the label's ink is at a point, 0 to 1 (the letters' coverage, read between their rows)
    float ink(float x, float y) const;

    // How wide the text is, in texels, for cap_height texels tall
    static float width_for(std::string_view text, float cap_height);

private:
    struct Placed {
        int glyph = -1;            // (key_letters_data.h's glyphs[]; -1: an arrow)
        char arrow = 0;
        float x0 = 0, y0 = 0;      // (its picture's corner, texels)
    };
    std::vector<Placed> placed_;
    float scale_;                  // (texels to a row of the letters' pictures)
    float left_, top_, right_, bottom_;
};

} // namespace pcr
