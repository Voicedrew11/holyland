// The keys' labels (key_letters.h): the font's letters read between their rows, the arrows as a shaft and a head.
#include "key_letters.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "key_letters_data.h"
#include "stroke_letters.h"

namespace pcr {
namespace {

namespace data = key_letters_data;

// An arrow, in rows of the letters' pictures: as wide as it is advanced, a capital tall
constexpr float arrow_advance = data::cap_height * 0.95f;
constexpr float arrow_shaft = 0.24f, arrow_head = 0.52f, arrow_head_width = 0.95f; // (shares of a capital's height)

bool is_arrow(char c) {
    return c == arrow_up || c == arrow_down || c == arrow_left || c == arrow_right;
}

int glyph_of(char c) {
    for (size_t i = 0; i < std::size(data::glyphs); i++) {
        if (data::glyphs[i].character == c) {
            return int(i);
        }
    }
    return -1;
}

// A character's advance, in rows (0: none to draw)
float advance_of(char c) {
    if (is_arrow(c)) {
        return arrow_advance;
    }
    int g = glyph_of(c);
    return g < 0 ? 0.0f : float(data::glyphs[g].advance);
}

// A glyph's coverage at a point of its picture (rows from its corner), 0 to 1, between its rows
float coverage_at(const data::Glyph &g, float u, float v) {
    u -= 0.5f;
    v -= 0.5f;
    const int x0 = int(std::floor(u)), y0 = int(std::floor(v));
    const float fx = u - float(x0), fy = v - float(y0);
    auto at = [&](int x, int y) -> float {
        if (x < 0 || y < 0 || x >= g.width || y >= g.height) {
            return 0.0f;
        }
        return float(data::coverage[g.start + size_t(y) * g.width + size_t(x)]) / 255.0f;
    };
    float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
    float bottom = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return top + (bottom - top) * fy;
}

// Whether a point is on an arrow pointing up, (a, b) across and along it from its middle in shares of its height (b
// up)
bool on_up_arrow(float a, float b) {
    const float tip = 0.5f, head_base = tip - arrow_head;
    if (b > tip || b < -0.5f) {
        return false;
    }
    if (b >= head_base) {
        return std::abs(a) <= (tip - b) / arrow_head * arrow_head_width / 2;
    }
    return std::abs(a) <= arrow_shaft / 2;
}

} // namespace

KeyText::KeyText(std::string_view text, float centre_x, float centre_y, float cap_height)
    : scale_(cap_height / float(data::cap_height)) {
    float pen = centre_x - width_for(text, cap_height) / 2;
    const float baseline = centre_y + cap_height / 2;
    left_ = top_ = INFINITY;
    right_ = bottom_ = -INFINITY;
    for (char c : text) {
        Placed p;
        float x1 = 0, y1 = 0;
        if (is_arrow(c)) {
            p.arrow = c;
            p.x0 = pen;
            p.y0 = baseline - cap_height;
            x1 = pen + arrow_advance * scale_;
            y1 = baseline;
        } else if (int g = glyph_of(c); g >= 0) {
            const data::Glyph &glyph = data::glyphs[g];
            p.glyph = g;
            p.x0 = pen + float(glyph.left) * scale_;
            p.y0 = baseline - float(glyph.top) * scale_;
            x1 = p.x0 + float(glyph.width) * scale_;
            y1 = p.y0 + float(glyph.height) * scale_;
        } else {
            continue;
        }
        pen += advance_of(c) * scale_;
        left_ = std::min(left_, p.x0);
        top_ = std::min(top_, p.y0);
        right_ = std::max(right_, x1);
        bottom_ = std::max(bottom_, y1);
        placed_.push_back(p);
    }
}

float KeyText::ink(float x, float y) const {
    if (x < left_ || x > right_ || y < top_ || y > bottom_) {
        return 0.0f;
    }
    float out = 0.0f;
    for (const Placed &p : placed_) {
        if (p.glyph >= 0) {
            const data::Glyph &g = data::glyphs[p.glyph];
            out = std::max(out, coverage_at(g, (x - p.x0) / scale_, (y - p.y0) / scale_));
            continue;
        }
        const float size = float(data::cap_height) * scale_;
        const float a = (x - (p.x0 + arrow_advance * scale_ / 2)) / size, b = -(y - (p.y0 + size / 2)) / size;
        bool on = false;
        switch (p.arrow) {
        case arrow_up: on = on_up_arrow(a, b); break;
        case arrow_down: on = on_up_arrow(a, -b); break;
        case arrow_right: on = on_up_arrow(b, a); break;
        default: on = on_up_arrow(b, -a); break;
        }
        out = std::max(out, on ? 1.0f : 0.0f);
    }
    return out;
}

float KeyText::width_for(std::string_view text, float cap_height) {
    float rows = 0.0f;
    for (char c : text) {
        rows += advance_of(c);
    }
    return rows * cap_height / float(data::cap_height);
}

} // namespace pcr
