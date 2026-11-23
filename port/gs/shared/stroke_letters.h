// Capital letters, digits, arrows and a few signs drawn as strokes, for the labels the buttons are drawn with in place
// of the PlayStation's symbols (button_pictures.h: the Xbox controller's letters, the keys bound). Each letter is
// lines and arcs on a grid four units wide and six tall; a new letter is a new case in stroke_letters.cpp. Two shapes:
// rounded (round-ended strokes) and angular (square-ended strokes cut flat at the letters' top and bottom, curves
// made of straight lines where an angular letter of its own is drawn).
//
// Plain data: any thread.
#pragma once

#include <string_view>
#include <vector>

namespace pcr {

// The arrows' characters in a label (as code page 437 has them)
constexpr char arrow_up = '\x18', arrow_down = '\x19', arrow_right = '\x1a', arrow_left = '\x1b';

enum class LetterShape { rounded, angular };

// A line of text laid out as strokes, in a picture's texels
class StrokeText {
public:
    // text (capitals; a character without a letter of its own is drawn as '?') centred on (centre_x, centre_y),
    // cap_height texels tall (its strokes' middles), its strokes stroke_width texels wide
    StrokeText(std::string_view text, float centre_x, float centre_y, float cap_height, float stroke_width,
               LetterShape shape);

    // Whether the point is on a stroke
    bool covers(float x, float y) const;

    // How wide the text is, in texels, for cap_height texels tall (the strokes' widths not counted)
    static float width_for(std::string_view text, float cap_height);

private:
    struct Segment {
        float x0, y0, x1, y1;
    };
    std::vector<Segment> segments_;
    float half_width_;
    LetterShape shape_;
    float left_ = 0, top_ = 0, right_ = 0, bottom_ = 0; // (the strokes' box with their width, for a quick miss)
};

} // namespace pcr
