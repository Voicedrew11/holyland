// The controller's buttons drawn again (button_pictures.h). The body's shading is the tool's model
// (tools/make_button_pictures.py): rings across the radius times waves round it, fitted by least squares to the
// body's texels the symbol leaves alone, bent as little as it can where the symbol hides it.
#include "button_pictures.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>

#include "button_pictures_data.h"
#include "key_letters.h"
#include "stroke_letters.h"

namespace pcr {
namespace {

namespace data = button_pictures_data;

constexpr uint32_t k = button_picture_scale;
constexpr uint32_t wave_terms = 2 * data::waves + 1;
constexpr uint32_t terms = data::rings * wave_terms;
constexpr uint32_t fit_samples = 6;  // (across a texel, when its share of the symbol and of each term is measured)
constexpr uint32_t draw_samples = 4; // (across a finer texel, for its share of the symbol and of the body's disc)
constexpr float body_margin = 0.5f;  // (texels round the symbol whose colour the fit does not take: its blur)

uint32_t channel(uint32_t colour, uint32_t c) {
    return (colour >> (8 * c)) & 0xFF;
}

using Terms = std::array<double, terms>;

// The shading model's terms at a point of the cell (texels from its corner)
Terms shading_terms(double x, double y) {
    double dx = x - data::centre, dy = y - data::centre;
    double length = std::sqrt(dx * dx + dy * dy);
    double r = std::min(length, double(data::radius));
    double fade = std::min(r / data::centre_fade, 1.0);
    // (cos and sin of w times the angle, from the angle's own by turning w times)
    double c1 = length > 0 ? dx / length : 1, s1 = length > 0 ? dy / length : 0;
    double cw = 1, sw = 0;
    std::array<double, wave_terms> waves{};
    waves[0] = 1;
    for (uint32_t w = 1; w <= data::waves; w++) {
        double c = cw * c1 - sw * s1;
        sw = sw * c1 + cw * s1;
        cw = c;
        waves[2 * w - 1] = fade * cw;
        waves[2 * w] = fade * sw;
    }
    Terms out{};
    for (uint32_t ring = 0; ring < data::rings; ring++) {
        // (each but the middle one mirrored through the middle, so it has no point there and even rings sum flat)
        double d = (r - ring * data::ring_step) / data::ring_sigma, e = (r + ring * data::ring_step) / data::ring_sigma;
        double g = std::exp(-0.5 * d * d) + (ring > 0 ? std::exp(-0.5 * e * e) : 0.0);
        for (uint32_t w = 0; w < wave_terms; w++) {
            out[ring * wave_terms + w] = g * waves[w];
        }
    }
    return out;
}

// How far inside a part's edge a point is (negative: outside)
double depth(const data::Part &part, double x, double y) {
    if (part.kind == data::PartKind::Ring) {
        double dx = x - part.x[0], dy = y - part.y[0];
        return part.x[1] - std::sqrt(dx * dx + dy * dy);
    }
    double area = 0;
    for (uint32_t i = 0; i < part.corners; i++) {
        uint32_t j = (i + 1) % part.corners;
        area += part.x[i] * part.y[j] - part.x[j] * part.y[i];
    }
    double turn = area > 0 ? 1 : -1;
    double d = INFINITY;
    for (uint32_t i = 0; i < part.corners; i++) {
        uint32_t j = (i + 1) % part.corners;
        double ex = part.x[j] - part.x[i], ey = part.y[j] - part.y[i];
        d = std::min(d, turn * (ex * (y - part.y[i]) - ey * (x - part.x[i])) / std::sqrt(ex * ex + ey * ey));
    }
    return d;
}

// Whether a point is the symbol's ink, its parts widened by grow each side
bool is_ink(const data::Button &button, double x, double y, double grow) {
    for (uint32_t p = 0; p < button.part_count; p++) {
        const data::Part &part = data::parts[button.first_part + p];
        double d = depth(part, x, y);
        if (d >= -grow && (part.stroke == 0 || d <= part.stroke + grow)) {
            return true;
        }
    }
    return false;
}

// One button's cell of a picture, its texels from (x0, y0); opaque its body's alpha, background its corner's
struct Cell {
    std::span<const uint32_t> colours;
    uint32_t width, x0, y0;
    uint32_t opaque = 0, background = 0;

    uint32_t at(uint32_t x, uint32_t y) const { return colours[size_t(y0 + y) * width + x0 + x]; }
    // (the hints' sheet is not quite clear round its buttons: the body is what is at least half its alpha)
    bool in_body(uint32_t x, uint32_t y) const { return 2 * channel(at(x, y), 3) >= opaque && opaque > 0; }
};

Cell cell_at(std::span<const uint32_t> colours, uint32_t width, uint32_t x0, uint32_t y0) {
    Cell cell{colours, width, x0, y0};
    for (uint32_t y = 0; y < data::cell; y++) {
        for (uint32_t x = 0; x < data::cell; x++) {
            cell.opaque = std::max(cell.opaque, channel(cell.at(x, y), 3));
        }
    }
    cell.background = channel(cell.at(0, 0), 3);
    return cell;
}

// The shading model's terms where every button's fit and drawing need them: the same for every button's cell
struct CellTerms {
    std::vector<Terms> texels;                    // (each texel's average of each term, rows top down)
    std::vector<std::array<float, terms>> finer;  // (at each finer texel's middle)
};

CellTerms cell_terms() {
    CellTerms out;
    out.texels.resize(size_t(data::cell) * data::cell);
    for (uint32_t y = 0; y < data::cell; y++) {
        for (uint32_t x = 0; x < data::cell; x++) {
            Terms &sum = out.texels[size_t(y) * data::cell + x];
            for (uint32_t sy = 0; sy < fit_samples; sy++) {
                for (uint32_t sx = 0; sx < fit_samples; sx++) {
                    Terms t = shading_terms(x + (sx + 0.5) / fit_samples, y + (sy + 0.5) / fit_samples);
                    for (uint32_t i = 0; i < terms; i++) {
                        sum[i] += t[i] / (fit_samples * fit_samples);
                    }
                }
            }
        }
    }
    const uint32_t n = data::cell * k;
    out.finer.resize(size_t(n) * n);
    for (uint32_t fy = 0; fy < n; fy++) {
        for (uint32_t fx = 0; fx < n; fx++) {
            Terms t = shading_terms((fx + 0.5) / k, (fy + 0.5) / k);
            std::copy(t.begin(), t.end(), out.finer[size_t(fy) * n + fx].begin());
        }
    }
    return out;
}

// Whether any of a texel is near the symbol
bool near_ink(const data::Button &button, uint32_t x, uint32_t y) {
    for (uint32_t sy = 0; sy < fit_samples; sy++) {
        for (uint32_t sx = 0; sx < fit_samples; sx++) {
            if (is_ink(button, x + (sx + 0.5) / fit_samples, y + (sy + 0.5) / fit_samples, body_margin)) {
                return true;
            }
        }
    }
    return false;
}

// The fit's penalty: bend on each wave's second difference across the rings, ridge on every term (the tool's)
void add_smoothing(std::vector<double> &normal) {
    for (uint32_t ring = 1; ring + 1 < data::rings; ring++) {
        for (uint32_t w = 0; w < wave_terms; w++) {
            std::array<uint32_t, 3> at = {(ring - 1) * wave_terms + w, ring * wave_terms + w, (ring + 1) * wave_terms + w};
            std::array<double, 3> d = {1, -2, 1};
            for (uint32_t a = 0; a < 3; a++) {
                for (uint32_t b = 0; b < 3; b++) {
                    normal[size_t(at[a]) * terms + at[b]] += data::bend * d[a] * d[b];
                }
            }
        }
    }
    for (uint32_t i = 0; i < terms; i++) {
        normal[size_t(i) * terms + i] += data::ridge;
    }
}

// Solves normal x = rhs (normal symmetric positive definite, rhs a column per colour) in place; false if it is not
bool cholesky_solve(std::vector<double> &normal, std::vector<std::array<double, 3>> &rhs) {
    const uint32_t n = terms;
    for (uint32_t j = 0; j < n; j++) {
        double d = normal[size_t(j) * n + j];
        for (uint32_t m = 0; m < j; m++) {
            d -= normal[size_t(j) * n + m] * normal[size_t(j) * n + m];
        }
        if (!(d > 0)) {
            return false;
        }
        double l = std::sqrt(d);
        normal[size_t(j) * n + j] = l;
        for (uint32_t i = j + 1; i < n; i++) {
            double s = normal[size_t(i) * n + j];
            for (uint32_t m = 0; m < j; m++) {
                s -= normal[size_t(i) * n + m] * normal[size_t(j) * n + m];
            }
            normal[size_t(i) * n + j] = s / l;
        }
    }
    for (uint32_t c = 0; c < 3; c++) {
        for (uint32_t i = 0; i < n; i++) { // (L y = b)
            double s = rhs[i][c];
            for (uint32_t m = 0; m < i; m++) {
                s -= normal[size_t(i) * n + m] * rhs[m][c];
            }
            rhs[i][c] = s / normal[size_t(i) * n + i];
        }
        for (uint32_t i = n; i-- > 0;) { // (L^T x = y)
            double s = rhs[i][c];
            for (uint32_t m = i + 1; m < n; m++) {
                s -= normal[size_t(m) * n + i] * rhs[m][c];
            }
            rhs[i][c] = s / normal[size_t(i) * n + i];
        }
    }
    return true;
}

// The body's shading, a coefficient per term and colour, fitted to the cell's body texels away from the symbol
std::optional<std::vector<std::array<double, 3>>> fit_body(const data::Button &button, const Cell &cell,
                                                            const CellTerms &shared) {
    std::vector<double> normal(size_t(terms) * terms, 0.0);
    std::vector<std::array<double, 3>> rhs(terms, {0, 0, 0});
    uint32_t used = 0;
    for (uint32_t y = 0; y < data::cell; y++) {
        for (uint32_t x = 0; x < data::cell; x++) {
            uint32_t texel = cell.at(x, y);
            if (!cell.in_body(x, y)) {
                continue;
            }
            if (near_ink(button, x, y)) {
                continue;
            }
            const Terms &t = shared.texels[size_t(y) * data::cell + x];
            used++;
            for (uint32_t i = 0; i < terms; i++) {
                for (uint32_t j = 0; j <= i; j++) {
                    normal[size_t(i) * terms + j] += t[i] * t[j];
                }
                for (uint32_t c = 0; c < 3; c++) {
                    rhs[i][c] += t[i] * channel(texel, c);
                }
            }
        }
    }
    if (used < data::cell) {
        return std::nullopt;
    }
    for (uint32_t i = 0; i < terms; i++) { // (the upper half from the lower)
        for (uint32_t j = i + 1; j < terms; j++) {
            normal[size_t(i) * terms + j] = normal[size_t(j) * terms + i];
        }
    }
    add_smoothing(normal);
    if (!cholesky_solve(normal, rhs)) {
        return std::nullopt;
    }
    return rhs;
}

uint32_t byte(double v) {
    return uint32_t(std::clamp(std::lround(v), 0l, 255l));
}

// Whether a point (texels from the cell's corner) is the symbol drawn in its place
using InkShape = std::function<bool(double x, double y)>;

// Where a finer texel of a cell is written (the cell's corner in the finer picture, fine_width wide)
size_t fine_index(const Cell &cell, uint32_t fx, uint32_t fy, uint32_t fine_width) {
    return size_t(cell.y0 * k + fy) * fine_width + cell.x0 * k + fx;
}

// The button's body, its shading fitted to the cell, drawn again into the finer picture with ink_shape over it in
// ink_colour
void draw_round_button(const data::Button &button, const Cell &cell, const CellTerms &shared, const InkShape &ink_shape,
                       std::array<double, 3> ink_colour, std::span<uint32_t> fine, uint32_t fine_width) {
    std::optional<std::vector<std::array<double, 3>>> body = fit_body(button, cell, shared);
    if (!body) {
        return;
    }
    for (uint32_t fy = 0; fy < data::cell * k; fy++) {
        for (uint32_t fx = 0; fx < data::cell * k; fx++) {
            const std::array<float, terms> &t = shared.finer[size_t(fy) * data::cell * k + fx];
            std::array<double, 3> shade{};
            for (uint32_t i = 0; i < terms; i++) {
                for (uint32_t c = 0; c < 3; c++) {
                    shade[c] += t[i] * (*body)[i][c];
                }
            }
            uint32_t inside = 0, ink = 0;
            for (uint32_t sy = 0; sy < draw_samples; sy++) {
                for (uint32_t sx = 0; sx < draw_samples; sx++) {
                    double x = (fx + (sx + 0.5) / draw_samples) / k, y = (fy + (sy + 0.5) / draw_samples) / k;
                    double dx = x - data::centre, dy = y - data::centre;
                    if (dx * dx + dy * dy > double(data::radius) * data::radius) {
                        continue;
                    }
                    inside++;
                    ink += ink_shape(x, y) ? 1 : 0;
                }
            }
            // (outside the disc the colour stays the body's, so the filter blends the edge with it, not with black)
            double share = inside ? double(ink) / inside : 0;
            double body_share = double(inside) / (draw_samples * draw_samples);
            uint32_t out = byte(cell.background + (double(cell.opaque) - cell.background) * body_share) << 24;
            for (uint32_t c = 0; c < 3; c++) {
                out |= byte(shade[c] * (1 - share) + ink_colour[c] * share) << (8 * c);
            }
            fine[fine_index(cell, fx, fy, fine_width)] = out;
        }
    }
}

// The keyboard's: a key as the PC versions of the late 2000s drew them, seen a little from above: a black cap, its
// top face raised and lit from above, its sides falling away darker (the front's the deepest), a dark rim and a soft
// shadow under it, the key's label printed light on the top face. All of it in the cell's texels.
struct RoundedBox {
    float x0, y0, x1, y1, radius;

    // How far outside its edge a point is (negative: inside)
    float distance(float x, float y) const {
        float dx = x - std::clamp(x, x0 + radius, x1 - radius), dy = y - std::clamp(y, y0 + radius, y1 - radius);
        return std::sqrt(dx * dx + dy * dy) - radius;
    }
    // Which way a point outside its middle part (the box shrunk by radius) is from it
    void towards(float x, float y, float &dx, float &dy) const {
        dx = x - std::clamp(x, x0 + radius, x1 - radius);
        dy = y - std::clamp(y, y0 + radius, y1 - radius);
    }
};

// A key one U wide (a letter key's) fills a button's cell; each U more adds the cell's width, as the keys of a real
// keyboard are spaced. Texels from the key's picture's corner.
constexpr float key_pitch = float(data::cell);        // (a U)
constexpr float key_gap = 3.0f;                       // (between two keys side by side)
constexpr float key_height = key_pitch - key_gap;
// How much of its sides shows (left and right, back, front: seen a little from the front) and how tall it stands
constexpr float key_side = 1.9f, key_back = 0.8f, key_front = 3.6f, key_tall = 2.6f;
constexpr float key_corner = 1.7f, key_top_corner = 1.5f;
constexpr float key_dish = 0.22f;                     // (how deep its top is scooped, side to side)
constexpr float key_shadow_dy = 0.7f, key_shadow_soft = 1.4f, key_shadow_alpha = 0.6f;
constexpr float key_label_margin = 1.6f, key_label_height = 7.0f; // (the most the label is)

// The plastic and the light, in linear light. Seen from the front and a little above; lit from above, behind and to
// the left, as a room light is.
struct KeyMaterial {
    double plastic, legend;          // (how much light each gives back, of what falls on it)
    double ambient;                  // (light from everywhere, against the light's 1)
    double gloss, sheen;             // (a sharp highlight's and a broad one's strength)
    double gloss_power, sheen_power;
};
constexpr KeyMaterial key_material = {0.012, 0.62, 0.45, 0.22, 0.012, 70.0, 6.0};
constexpr std::array<double, 3> key_light = {-0.38, -0.62, 0.69};  // (x right, y down the picture, z toward the eye)
constexpr std::array<double, 3> key_eye = {0.0, 0.42, 0.91};
constexpr std::array<double, 3> key_tint = {1.0, 1.0, 1.04};       // (the plastic's: a little cool)

std::array<double, 3> normalised(std::array<double, 3> v) {
    double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / length, v[1] / length, v[2] / length};
}

double dot(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

uint32_t srgb_byte(double linear) {
    linear = std::clamp(linear, 0.0, 1.0);
    double v = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
    return byte(255 * v);
}

// One key, width_u U wide, its corner at (x0, y0): its body (where it meets the board), its top face and its label
struct Key {
    RoundedBox body, top;
    KeyText label;

    Key(std::string_view text, float width_u, float x0, float y0)
        : body{x0 + key_gap / 2, y0 + key_gap / 2, x0 + key_gap / 2 + key_pitch * width_u - key_gap,
               y0 + key_gap / 2 + key_height, key_corner},
          top{body.x0 + key_side, body.y0 + key_back, body.x1 - key_side, body.y1 - key_front, key_top_corner},
          label(fitted_label(text, top)) {}

    static KeyText fitted_label(std::string_view text, const RoundedBox &top) {
        float height = key_label_height, room = top.x1 - top.x0 - 2 * key_label_margin;
        if (float unit_width = KeyText::width_for(text, 1); unit_width > 0) {
            height = std::min(height, room / unit_width);
        }
        return KeyText(text, (top.x0 + top.x1) / 2, (top.y0 + top.y1) / 2, height);
    }

    // How high the key is at a point inside its body, texels above the board
    float height(float x, float y) const {
        float outside_top = top.distance(x, y);
        if (outside_top <= 0) {
            float across = (2 * x - top.x0 - top.x1) / (top.x1 - top.x0);
            return key_tall - key_dish * (1 - across * across);
        }
        float inside_body = std::max(-body.distance(x, y), 0.0f);
        float s = inside_body / (inside_body + outside_top); // (0 at the board, 1 at the top face's edge)
        // (steep near the board, rounding over into the top)
        return key_tall * float(1 - std::pow(1 - s, 2.4));
    }

    // The key's colour at a point inside its body, linear
    std::array<double, 3> colour(float x, float y) const {
        const KeyMaterial &m = key_material;
        constexpr float e = 0.12f;
        std::array<double, 3> normal = normalised({-(height(x + e, y) - height(x - e, y)) / (2 * e),
                                                   -(height(x, y + e) - height(x, y - e)) / (2 * e), 1.0});
        const std::array<double, 3> light = normalised(key_light), eye = normalised(key_eye);
        const std::array<double, 3> half = normalised({light[0] + eye[0], light[1] + eye[1], light[2] + eye[2]});
        double ink = top.distance(x, y) <= 0 ? label.ink(x, y) : 0.0;
        double albedo = m.plastic + (m.legend - m.plastic) * ink;
        double facing = std::max(dot(normal, light), 0.0), towards_half = std::max(dot(normal, half), 0.0);
        // (darker where the key meets the board: the light it lets past)
        double occlusion = 0.45 + 0.55 * std::clamp(double(height(x, y)) / (0.6 * key_tall), 0.0, 1.0);
        double diffuse = albedo * (m.ambient * (0.6 + 0.4 * normal[2]) + facing) * occlusion;
        double shine = (m.gloss * (1 - ink * 0.7) * std::pow(towards_half, m.gloss_power) +
                        m.sheen * std::pow(towards_half, m.sheen_power)) *
                       occlusion;
        return {diffuse * key_tint[0] + shine, diffuse * key_tint[1] + shine, diffuse * key_tint[2] + shine};
    }

    // A finer texel's colour and how much of it the key and its shadow cover (0 to 1); (x, y) its corner, size its
    // width, in texels
    uint32_t texel(float x, float y, float size, double &share) const {
        std::array<double, 3> sum{};
        double cover = 0, shadow = 0;
        for (uint32_t sy = 0; sy < draw_samples; sy++) {
            for (uint32_t sx = 0; sx < draw_samples; sx++) {
                float px = x + size * (sx + 0.5f) / draw_samples, py = y + size * (sy + 0.5f) / draw_samples;
                if (body.distance(px, py) <= 0) {
                    std::array<double, 3> c = colour(px, py);
                    for (uint32_t i = 0; i < 3; i++) {
                        sum[i] += c[i];
                    }
                    cover++;
                    continue;
                }
                float d = body.distance(px, py - key_shadow_dy);
                float fade = std::clamp(1.0f - d / key_shadow_soft, 0.0f, 1.0f);
                shadow += key_shadow_alpha * fade * fade;
            }
        }
        share = (cover + shadow) / (draw_samples * draw_samples);
        // (the shadow is black; beyond the key and its shadow the colour is the plastic's, for the filter to blend
        // toward)
        uint32_t out = 0;
        for (uint32_t i = 0; i < 3; i++) {
            out |= srgb_byte(cover + shadow > 0 ? sum[i] / (cover + shadow) : key_material.plastic) << (8 * i);
        }
        return out;
    }
};

// A one-U key drawn into a button's cell of the finer picture: the key opaque as the button's body was, its shadow
// part way, the picture's background beyond
void draw_key_in_cell(const Cell &cell, std::string_view text, std::span<uint32_t> fine, uint32_t fine_width) {
    const Key key(text, 1.0f, 0, 0);
    for (uint32_t fy = 0; fy < data::cell * k; fy++) {
        for (uint32_t fx = 0; fx < data::cell * k; fx++) {
            double share = 0;
            uint32_t out = key.texel(float(fx) / k, float(fy) / k, 1.0f / k, share);
            out |= byte(cell.background + (double(cell.opaque) - cell.background) * share) << 24;
            fine[fine_index(cell, fx, fy, fine_width)] = out;
        }
    }
}

std::optional<FaceButton> face_button_of(const data::Button &button) {
    static constexpr std::array<std::string_view, face_button_count> names = {"cross", "circle", "square", "triangle"};
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] == button.symbol) {
            return FaceButton(i);
        }
    }
    return std::nullopt;
}

// The Xbox controller's letter in each face button's place, and its colour (a little softer than the controller's, as
// the disc's symbols are)
struct XboxLetter {
    const char *letter;
    std::array<double, 3> colour;
};
constexpr std::array<XboxLetter, face_button_count> xbox_letters = {{
    {"A", {118, 196, 92}},  // (cross: south)
    {"B", {222, 92, 80}},   // (circle: east)
    {"X", {92, 148, 228}},  // (square: west)
    {"Y", {232, 200, 84}},  // (triangle: north)
}};
constexpr float xbox_letter_height = 10.0f, xbox_letter_stroke = 2.0f; // (texels)

// One button drawn again in a style
struct Redraw {
    const data::Button &button;
    FaceButton face;
    const Cell &cell;
    const CellTerms &shared;
    const ButtonLooks &looks;
    std::span<uint32_t> fine;
    uint32_t fine_width;
};

void draw_playstation(const Redraw &r) {
    const std::array<double, 3> colour = {double(r.button.r), double(r.button.g), double(r.button.b)};
    draw_round_button(r.button, r.cell, r.shared, [&](double x, double y) { return is_ink(r.button, x, y, 0); }, colour,
                      r.fine, r.fine_width);
}

void draw_xbox(const Redraw &r) {
    const XboxLetter &letter = xbox_letters[size_t(r.face)];
    const StrokeText text(letter.letter, float(data::centre), float(data::centre), xbox_letter_height,
                          xbox_letter_stroke, LetterShape::angular);
    draw_round_button(r.button, r.cell, r.shared, [&](double x, double y) { return text.covers(float(x), float(y)); },
                      letter.colour, r.fine, r.fine_width);
}

void draw_keyboard(const Redraw &r) {
    draw_key_in_cell(r.cell, key_label_text(r.looks.keys[size_t(r.face)]), r.fine, r.fine_width);
}

struct Style {
    ButtonStyle style;
    void (*draw)(const Redraw &);
};
constexpr std::array<Style, 3> styles = {{
    {ButtonStyle::playstation, draw_playstation},
    {ButtonStyle::xbox, draw_xbox},
    {ButtonStyle::keyboard, draw_keyboard},
}};

const Style *style_of(ButtonStyle style) {
    for (const Style &s : styles) {
        if (s.style == style) {
            return &s;
        }
    }
    return nullptr;
}

uint64_t indices_hash(const DecodedTexture &texture) {
    uint64_t h = 0xcbf29ce484222325ull;
    size_t n = size_t(texture.width_px) * texture.height_px;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ texture.indices[i]) * 0x100000001b3ull;
    }
    return h;
}

} // namespace

uint64_t ButtonLooks::hash() const {
    if (style == ButtonStyle::disc) {
        return 0;
    }
    uint64_t h = (0xcbf29ce484222325ull ^ uint64_t(style)) * 0x100000001b3ull;
    if (style == ButtonStyle::keyboard) { // (only the keyboard's buttons show the keys)
        for (const std::string &key : keys) {
            for (char c : key) {
                h = (h ^ uint8_t(c)) * 0x100000001b3ull;
            }
            h = (h ^ 0xFF) * 0x100000001b3ull;
        }
    }
    return h;
}

std::string key_label_text(std::string_view key_name) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 30> named = {{
        {"Escape", "ESC"},       {"Space", "SPACE"},     {"Tab", "TAB"},           {"Return", "ENTER"},
        {"Keypad Enter", "ENTER"}, {"Backspace", "BKSP"}, {"Left Shift", "SHIFT"},  {"Right Shift", "SHIFT"},
        {"Left Ctrl", "CTRL"},   {"Right Ctrl", "CTRL"}, {"Left Alt", "ALT"},      {"Right Alt", "ALT"},
        {"Left GUI", "SUPER"},   {"Right GUI", "SUPER"}, {"CapsLock", "CAPS"},     {"Delete", "DEL"},
        {"Insert", "INS"},       {"Home", "HOME"},       {"End", "END"},           {"PageUp", "PGUP"},
        {"PageDown", "PGDN"},    {"Up", "\x18"},        {"Down", "\x19"},        {"Right", "\x1a"},
        {"Left", "\x1b"},       {"Mouse left", "LMB"},  {"Mouse right", "RMB"},   {"Mouse middle", "MMB"},
        {"Mouse 4", "M4"},       {"Mouse 5", "M5"},
    }};
    for (const auto &[name, text] : named) {
        if (name == key_name) {
            return std::string(text);
        }
    }
    // (the keypad's keys by their own: "Keypad 5" 5)
    constexpr std::string_view keypad = "Keypad ";
    if (key_name.substr(0, keypad.size()) == keypad) {
        key_name.remove_prefix(keypad.size());
    }
    std::string out;
    for (char c : key_name.substr(0, 5)) {
        out += char(std::toupper(uint8_t(c)));
    }
    return out;
}

float key_width_u(std::string_view key_name) {
    // (a US keyboard's ANSI widths, but Space: 3 U, not 6.25, so a hint with it still fits a line)
    static constexpr std::array<std::pair<std::string_view, float>, 14> widths = {{
        {"Tab", 1.5f},          {"CapsLock", 1.75f},   {"Backspace", 2.0f},  {"Return", 2.25f},
        {"Left Shift", 2.25f},  {"Right Shift", 2.75f}, {"Left Ctrl", 1.25f}, {"Right Ctrl", 1.25f},
        {"Left Alt", 1.25f},    {"Right Alt", 1.25f},   {"Left GUI", 1.25f},  {"Right GUI", 1.25f},
        {"Space", 3.0f},        {"Keypad 0", 2.0f},
    }};
    for (const auto &[name, width] : widths) {
        if (name == key_name) {
            return width;
        }
    }
    return 1.0f;
}

KeyPicture key_picture(std::string_view key_name, uint32_t scale) {
    const float width_u = key_width_u(key_name);
    KeyPicture out;
    out.width = uint32_t(std::ceil(width_u * data::cell)) * scale;
    out.height = data::cell * scale;
    out.texels.resize(size_t(out.width) * out.height);
    const Key key(key_label_text(key_name), width_u, 0, 0);
    for (uint32_t y = 0; y < out.height; y++) {
        for (uint32_t x = 0; x < out.width; x++) {
            double share = 0;
            uint32_t colour = key.texel(float(x) / scale, float(y) / scale, 1.0f / scale, share);
            out.texels[size_t(y) * out.width + x] = colour | byte(255 * share) << 24;
        }
    }
    return out;
}

bool may_have_buttons(const DecodedTexture &texture) {
    if (texture.kind != TexelKind::Index8 || texture.levels != 1 ||
        texture.indices.size() < size_t(texture.width_px) * texture.height_px) {
        return false;
    }
    return std::any_of(std::begin(data::pictures), std::end(data::pictures), [&](const data::Picture &p) {
        return p.width == texture.width_px && p.height == texture.height_px;
    });
}

namespace {

// The table's picture a texture is, by its size and its indices' hash
std::optional<size_t> listed_picture(const DecodedTexture &texture) {
    if (!may_have_buttons(texture)) {
        return std::nullopt;
    }
    uint64_t h = indices_hash(texture);
    for (size_t i = 0; i < std::size(data::pictures); i++) {
        const data::Picture &p = data::pictures[i];
        if (p.width == texture.width_px && p.height == texture.height_px && p.hash == h) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace

bool has_buttons(const DecodedTexture &texture) {
    return listed_picture(texture).has_value();
}

bool draw_buttons(const DecodedTexture &texture, std::span<const uint32_t> colours, std::span<uint32_t> fine,
                  const ButtonLooks &looks) {
    std::optional<size_t> picture = listed_picture(texture);
    return picture && draw_listed_buttons(*picture, colours, texture.width_px, texture.height_px, fine, looks);
}

bool draw_listed_buttons(size_t picture, std::span<const uint32_t> colours, uint32_t width, uint32_t height,
                         std::span<uint32_t> fine, const ButtonLooks &looks) {
    const Style *style = style_of(looks.style);
    if (picture >= std::size(data::pictures) || !style) {
        return false;
    }
    const data::Picture &p = data::pictures[picture];
    if (p.width != width || p.height != height || colours.size() != size_t(width) * height ||
        fine.size() != size_t(width) * k * height * k) {
        return false;
    }
    const CellTerms shared = cell_terms();
    for (uint32_t b = 0; b < p.button_count; b++) {
        const data::Button &button = data::buttons[p.first_button + b];
        std::optional<FaceButton> face = face_button_of(button);
        if (!face) {
            continue;
        }
        const Cell cell = cell_at(colours, width, button.cell_x, button.cell_y);
        style->draw({button, *face, cell, shared, looks, fine, width * k});
    }
    return true;
}

} // namespace pcr
