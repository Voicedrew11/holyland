// The stroke letters (stroke_letters.h): each a few paths of lines and arcs on a grid four units wide, six tall, y
// down; arcs' angles in degrees from +x towards +y (so 90 is the bottom, 270 the top).
#include "stroke_letters.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace pcr {
namespace {

constexpr float grid_width = 4, grid_height = 6;
constexpr float spacing = 1.8f; // (units between letters)
constexpr float arc_step_deg = 10;

struct Point {
    float x, y;
};
using Path = std::vector<Point>;
using Letter = std::vector<Path>;

// A path built a piece at a time: lines to points and arcs, each joined to the last
class PathBuilder {
public:
    PathBuilder &to(float x, float y) {
        path_.push_back({x, y});
        return *this;
    }
    // An arc of the ellipse round (cx, cy) of radii (rx, ry), from angle a0 to a1 (either way round)
    PathBuilder &arc(float cx, float cy, float rx, float ry, float a0, float a1) {
        int steps = std::max(2, int(std::ceil(std::fabs(a1 - a0) / arc_step_deg)));
        for (int i = 0; i <= steps; i++) {
            float a = (a0 + (a1 - a0) * float(i) / float(steps)) * float(M_PI) / 180.0f;
            path_.push_back({cx + rx * std::cos(a), cy + ry * std::sin(a)});
        }
        return *this;
    }
    operator Path() const { return path_; }

private:
    Path path_;
};

PathBuilder line(std::initializer_list<Point> points) {
    PathBuilder b;
    for (Point p : points) {
        b.to(p.x, p.y);
    }
    return b;
}

PathBuilder arc(float cx, float cy, float rx, float ry, float a0, float a1) {
    return PathBuilder().arc(cx, cy, rx, ry, a0, a1);
}

Path dot(float x, float y) {
    return {{x, y}, {x, y + 0.05f}};
}

// The angular letters that differ from the rounded ones: curves made of straight lines
std::optional<Letter> angular_letter(char c) {
    switch (c) {
    case 'B':
        return Letter{line({{0, 6}, {0, 0}, {3, 0}, {3.8f, 0.8f}, {3.8f, 2.2f}, {3, 3}, {0, 3}}),
                      line({{3, 3}, {4, 3.9f}, {4, 5.1f}, {3.1f, 6}, {0, 6}})};
    default: return std::nullopt;
    }
}

Letter letter(char c) {
    switch (c) {
    case 'A': return {line({{0, 6}, {2, 0}, {4, 6}}), line({{0.7f, 4}, {3.3f, 4}})};
    case 'B':
        return {line({{0, 6}, {0, 0}, {2.4f, 0}}).arc(2.4f, 1.5f, 1.5f, 1.5f, -90, 90).to(0, 3),
                line({{0, 3}, {2.5f, 3}}).arc(2.5f, 4.5f, 1.5f, 1.5f, -90, 90).to(0, 6)};
    case 'C': return {arc(2.2f, 3, 2.2f, 3, -40, -320)};
    case 'D': return {line({{0, 6}, {0, 0}, {1.6f, 0}}).arc(1.6f, 3, 2.4f, 3, -90, 90).to(0, 6)};
    case 'E': return {line({{4, 0}, {0, 0}, {0, 6}, {4, 6}}), line({{0, 3}, {3, 3}})};
    case 'F': return {line({{4, 0}, {0, 0}, {0, 6}}), line({{0, 3}, {3, 3}})};
    case 'G': return {arc(2.2f, 3, 2.2f, 3, -40, -360).to(4.4f, 3.4f), line({{4.4f, 3.4f}, {2.4f, 3.4f}})};
    case 'H': return {line({{0, 0}, {0, 6}}), line({{4, 0}, {4, 6}}), line({{0, 3}, {4, 3}})};
    case 'I': return {line({{2, 0}, {2, 6}}), line({{1, 0}, {3, 0}}), line({{1, 6}, {3, 6}})};
    case 'J': return {line({{4, 0}, {4, 4}}).arc(2, 4, 2, 2, 0, 180)};
    case 'K': return {line({{0, 0}, {0, 6}}), line({{4, 0}, {0, 4}}), line({{1.4f, 2.9f}, {4, 6}})};
    case 'L': return {line({{0, 0}, {0, 6}, {4, 6}})};
    case 'M': return {line({{0, 6}, {0, 0}, {2, 4}, {4, 0}, {4, 6}})};
    case 'N': return {line({{0, 6}, {0, 0}, {4, 6}, {4, 0}})};
    case 'O': return {arc(2, 3, 2, 3, 0, 360)};
    case 'P': return {line({{0, 6}, {0, 0}, {2.4f, 0}}).arc(2.4f, 1.6f, 1.6f, 1.6f, -90, 90).to(0, 3.2f)};
    case 'Q': return {arc(2, 3, 2, 3, 0, 360), line({{2.6f, 4.4f}, {4.2f, 6.2f}})};
    case 'R':
        return {line({{0, 6}, {0, 0}, {2.4f, 0}}).arc(2.4f, 1.6f, 1.6f, 1.6f, -90, 90).to(0, 3.2f),
                line({{2.2f, 3.2f}, {4, 6}})};
    case 'S': return {arc(2, 1.5f, 1.9f, 1.5f, -30, -270).arc(2, 4.5f, 2, 1.5f, -90, 150)};
    case 'T': return {line({{0, 0}, {4, 0}}), line({{2, 0}, {2, 6}})};
    case 'U': return {line({{0, 0}, {0, 4}}).arc(2, 4, 2, 2, 180, 0).to(4, 0)};
    case 'V': return {line({{0, 0}, {2, 6}, {4, 0}})};
    case 'W': return {line({{0, 0}, {1, 6}, {2, 2}, {3, 6}, {4, 0}})};
    case 'X': return {line({{0, 0}, {4, 6}}), line({{4, 0}, {0, 6}})};
    case 'Y': return {line({{0, 0}, {2, 3}, {4, 0}}), line({{2, 3}, {2, 6}})};
    case 'Z': return {line({{0, 0}, {4, 0}, {0, 6}, {4, 6}})};
    case '0': return {arc(2, 3, 2, 3, 0, 360), line({{3.2f, 1}, {0.8f, 5}})};
    case '1': return {line({{0.8f, 1.2f}, {2, 0}, {2, 6}}), line({{0.8f, 6}, {3.2f, 6}})};
    case '2': return {arc(2, 1.9f, 2, 1.9f, 200, 380).to(0, 6).to(4, 6)};
    case '3': return {arc(2, 1.5f, 1.9f, 1.5f, 200, 450), arc(2, 4.5f, 2, 1.5f, 270, 520)};
    case '4': return {line({{3, 6}, {3, 0}, {0, 4.2f}, {4, 4.2f}})};
    case '5': return {line({{4, 0}, {0.3f, 0}, {0, 2.9f}}).arc(2, 4.2f, 2, 1.8f, 235, 510)};
    case '6': return {arc(2, 3, 2, 3, -60, -210), arc(2, 4.2f, 2, 1.8f, 0, 360)};
    case '7': return {line({{0, 0}, {4, 0}, {1.4f, 6}})};
    case '8': return {arc(2, 1.5f, 1.7f, 1.5f, 0, 360), arc(2, 4.5f, 2, 1.5f, 0, 360)};
    case '9': return {arc(2, 1.8f, 2, 1.8f, 0, 360), arc(2, 3, 2, 3, -20, 120)};
    case '-': return {line({{0.6f, 3}, {3.4f, 3}})};
    case '+': return {line({{0.4f, 3}, {3.6f, 3}}), line({{2, 1.4f}, {2, 4.6f}})};
    case '=': return {line({{0.4f, 2.2f}, {3.6f, 2.2f}}), line({{0.4f, 3.8f}, {3.6f, 3.8f}})};
    case '.': return {dot(2, 5.9f)};
    case ',': return {line({{2.3f, 5.5f}, {1.6f, 6.8f}})};
    case ';': return {dot(2, 2), line({{2.3f, 5.5f}, {1.6f, 6.8f}})};
    case '\'': return {line({{2, 0}, {2, 1.6f}})};
    case '`': return {line({{1.4f, 0}, {2.4f, 1.2f}})};
    case '/': return {line({{4, 0}, {0, 6}})};
    case '\\': return {line({{0, 0}, {4, 6}})};
    case '[': return {line({{3, 0}, {1.4f, 0}, {1.4f, 6}, {3, 6}})};
    case ']': return {line({{1, 0}, {2.6f, 0}, {2.6f, 6}, {1, 6}})};
    case arrow_up: return {line({{2, 6}, {2, 0}}), line({{0.2f, 2}, {2, 0}, {3.8f, 2}})};
    case arrow_down: return {line({{2, 0}, {2, 6}}), line({{0.2f, 4}, {2, 6}, {3.8f, 4}})};
    case arrow_right: return {line({{0, 3}, {4, 3}}), line({{2, 1.2f}, {4, 3}, {2, 4.8f}})};
    case arrow_left: return {line({{4, 3}, {0, 3}}), line({{2, 1.2f}, {0, 3}, {2, 4.8f}})};
    default: return {arc(2, 1.6f, 1.8f, 1.6f, 180, 450).to(2, 4.4f), dot(2, 5.9f)}; // ('?')
    }
}

float units_wide(std::string_view text) {
    return text.empty() ? 0 : float(text.size()) * (grid_width + spacing) - spacing;
}

// Whether a point is on a square-ended stroke: within half_width of the segment's line, and of its length widened
// by half_width each end
bool on_square_stroke(float x0, float y0, float x1, float y1, float half_width, float x, float y) {
    float dx = x1 - x0, dy = y1 - y0;
    float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0) {
        return std::fabs(x - x0) <= half_width && std::fabs(y - y0) <= half_width;
    }
    float along = ((x - x0) * dx + (y - y0) * dy) / length, across = ((x - x0) * dy - (y - y0) * dx) / length;
    return along >= -half_width && along <= length + half_width && std::fabs(across) <= half_width;
}

float distance_squared_to(float x0, float y0, float x1, float y1, float x, float y) {
    float dx = x1 - x0, dy = y1 - y0;
    float length2 = dx * dx + dy * dy;
    float t = length2 > 0 ? std::clamp(((x - x0) * dx + (y - y0) * dy) / length2, 0.0f, 1.0f) : 0.0f;
    float ex = x - (x0 + t * dx), ey = y - (y0 + t * dy);
    return ex * ex + ey * ey;
}

} // namespace

StrokeText::StrokeText(std::string_view text, float centre_x, float centre_y, float cap_height, float stroke_width,
                       LetterShape shape)
    : half_width_(stroke_width / 2), shape_(shape) {
    const float scale = cap_height / grid_height;
    float x = centre_x - units_wide(text) * scale / 2;
    const float y = centre_y - cap_height / 2;
    for (char c : text) {
        std::optional<Letter> angular = shape == LetterShape::angular ? angular_letter(c) : std::nullopt;
        for (const Path &path : angular ? *angular : letter(c)) {
            for (size_t i = 0; i < path.size(); i++) {
                const Point &a = path[i], &b = path[std::min(i + 1, path.size() - 1)];
                segments_.push_back({x + a.x * scale, y + a.y * scale, x + b.x * scale, y + b.y * scale});
            }
        }
        x += (grid_width + spacing) * scale;
    }
    if (segments_.empty()) {
        return;
    }
    left_ = top_ = INFINITY;
    right_ = bottom_ = -INFINITY;
    for (const Segment &s : segments_) {
        left_ = std::min({left_, s.x0, s.x1});
        right_ = std::max({right_, s.x0, s.x1});
        top_ = std::min({top_, s.y0, s.y1});
        bottom_ = std::max({bottom_, s.y0, s.y1});
    }
    left_ -= half_width_;
    right_ += half_width_;
    if (shape == LetterShape::angular) {
        // (cut flat at the letters' own top and bottom: slanted strokes' ends level with the bars')
        top_ = std::min(top_, y) - half_width_;
        bottom_ = std::max(bottom_, y + cap_height) + half_width_;
        left_ -= half_width_; // (a square end's corner reaches past half_width across)
        right_ += half_width_;
        return;
    }
    top_ -= half_width_;
    bottom_ += half_width_;
}

bool StrokeText::covers(float x, float y) const {
    if (x < left_ || x > right_ || y < top_ || y > bottom_) {
        return false;
    }
    if (shape_ == LetterShape::angular) {
        return std::any_of(segments_.begin(), segments_.end(), [&](const Segment &s) {
            return on_square_stroke(s.x0, s.y0, s.x1, s.y1, half_width_, x, y);
        });
    }
    const float reach2 = half_width_ * half_width_;
    return std::any_of(segments_.begin(), segments_.end(), [&](const Segment &s) {
        return distance_squared_to(s.x0, s.y0, s.x1, s.y1, x, y) <= reach2;
    });
}

float StrokeText::width_for(std::string_view text, float cap_height) {
    return units_wide(text) * cap_height / grid_height;
}

} // namespace pcr
