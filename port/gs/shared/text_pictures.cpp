// The game's word pictures made sharper (text_pictures.h).
#include "text_pictures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "finer_pictures.h"
#include "text_pictures_data.h"

namespace pcr {
namespace {

namespace data = text_pictures_data;

constexpr uint32_t k = text_picture_scale;
static_assert(data::scale == text_picture_scale, "text_pictures_data.h made at another scale");

// (texels beside the word's box each side whose glow the gap's is fitted to)
constexpr uint32_t glow_beside = 24;
// (texels round the word's ink painted over with it: the GS's filter and the game's own edge spread it so far)
constexpr uint32_t ink_margin = 2;

uint32_t channel(uint32_t colour, uint32_t c) {
    return (colour >> (8 * c)) & 0xFF;
}

// Whether a texel is a word's ink: bright and grey (the glow is blue, the hints' buttons coloured or dark)
bool is_ink(uint32_t colour) {
    uint32_t r = channel(colour, 0), g = channel(colour, 1), b = channel(colour, 2);
    uint32_t lo = std::min({r, g, b}), hi = std::max({r, g, b});
    return lo > 150 && hi - lo < 45;
}

uint32_t brightness(uint32_t colour) {
    return channel(colour, 0) + channel(colour, 1) + channel(colour, 2);
}

// A rectangle of texels or finer texels, x1 and y1 past its last
struct Box {
    uint32_t x0, y0, x1, y1;
};

// A picture being drawn: its colours, width x height
struct Canvas {
    std::span<uint32_t> colours;
    uint32_t width, height;

    uint32_t &at(uint32_t x, uint32_t y) const { return colours[size_t(y) * width + x]; }
};

// Where the game's word lies inside its region, and its ink's colour (its brightest texel's)
struct OldWord {
    Box ink;
    uint32_t colour;
};

std::optional<OldWord> find_old_word(const Canvas &c, const data::Word &w) {
    Box ink = {UINT32_MAX, UINT32_MAX, 0, 0};
    uint32_t colour = 0;
    for (uint32_t y = w.y0; y < std::min<uint32_t>(w.y1, c.height); y++) {
        for (uint32_t x = w.x0; x < std::min<uint32_t>(w.x1, c.width); x++) {
            uint32_t texel = c.at(x, y);
            if (!is_ink(texel)) {
                continue;
            }
            ink = {std::min(ink.x0, x), std::min(ink.y0, y), std::max(ink.x1, x + 1), std::max(ink.y1, y + 1)};
            if (brightness(texel) > brightness(colour)) {
                colour = texel;
            }
        }
    }
    if (ink.x0 == UINT32_MAX) {
        return std::nullopt;
    }
    return OldWord{ink, colour};
}

// The box painted over: the ink and its margin, inside the word's region and the picture
Box painted_box(const Canvas &c, const data::Word &w, const Box &ink) {
    auto grown = [](uint32_t v, uint32_t lo) { return v > lo + ink_margin ? v - ink_margin : lo; };
    return {grown(ink.x0, w.x0), grown(ink.y0, w.y0),
            std::min({ink.x1 + ink_margin, uint32_t(w.x1), c.width}),
            std::min({ink.y1 + ink_margin, uint32_t(w.y1), c.height})};
}

// The colour most of a box's texels that are not ink have; none when all are
std::optional<uint32_t> most_common_ground(const Canvas &c, const Box &b) {
    std::unordered_map<uint32_t, uint32_t> counts;
    uint32_t best = 0, best_count = 0;
    for (uint32_t y = b.y0; y < b.y1; y++) {
        for (uint32_t x = b.x0; x < b.x1; x++) {
            uint32_t texel = c.at(x, y);
            if (is_ink(texel)) {
                continue;
            }
            uint32_t n = ++counts[texel];
            if (n > best_count || (n == best_count && texel < best)) {
                best = texel;
                best_count = n;
            }
        }
    }
    if (best_count == 0) {
        return std::nullopt;
    }
    return best;
}

void paint_plain(const Canvas &c, const Box &b) {
    std::optional<uint32_t> colour = most_common_ground(c, b);
    if (!colour) {
        return;
    }
    for (uint32_t y = b.y0; y < b.y1; y++) {
        for (uint32_t x = b.x0; x < b.x1; x++) {
            c.at(x, y) = *colour;
        }
    }
}

// The quadratic a + b t + c t^2 nearest the points (least squares), t = x - middle
struct Quadratic {
    double a = 0, b = 0, c = 0, middle = 0;

    double at(double x) const {
        double t = x - middle;
        return a + b * t + c * t * t;
    }
};

std::optional<Quadratic> fit_quadratic(std::span<const double> xs, std::span<const double> ys, double middle) {
    // (the normal equations: sums of t^0..t^4 and of y t^0..t^2)
    std::array<double, 5> st{};
    std::array<double, 3> sy{};
    for (size_t i = 0; i < xs.size(); i++) {
        double t = xs[i] - middle, p = 1;
        for (size_t n = 0; n < 5; n++) {
            st[n] += p;
            if (n < 3) {
                sy[n] += ys[i] * p;
            }
            p *= t;
        }
    }
    std::array<std::array<double, 4>, 3> m = {{{st[0], st[1], st[2], sy[0]},
                                               {st[1], st[2], st[3], sy[1]},
                                               {st[2], st[3], st[4], sy[2]}}};
    for (size_t col = 0; col < 3; col++) {
        size_t pivot = col;
        for (size_t r = col + 1; r < 3; r++) {
            if (std::abs(m[r][col]) > std::abs(m[pivot][col])) {
                pivot = r;
            }
        }
        if (std::abs(m[pivot][col]) < 1e-9) {
            return std::nullopt;
        }
        std::swap(m[col], m[pivot]);
        for (size_t r = 0; r < 3; r++) {
            if (r == col) {
                continue;
            }
            double f = m[r][col] / m[col][col];
            for (size_t j = col; j < 4; j++) {
                m[r][j] -= f * m[col][j];
            }
        }
    }
    return Quadratic{m[0][3] / m[0][0], m[1][3] / m[1][1], m[2][3] / m[2][2], middle};
}

// Paints a box with the glow around it, one channel at a time: each column's sum over the box's rows from a curve
// fitted to the columns beside it, and each row's share of a column as the columns beside it have it. False when there
// are no columns beside it to fit (the box left as it is).
bool paint_glow(const Canvas &c, const Box &b) {
    std::vector<uint32_t> beside;
    for (uint32_t x = b.x0 > glow_beside ? b.x0 - glow_beside : 0; x < b.x0; x++) {
        beside.push_back(x);
    }
    for (uint32_t x = b.x1; x < std::min(c.width, b.x1 + glow_beside); x++) {
        beside.push_back(x);
    }
    if (beside.size() < 3) {
        return false;
    }
    std::vector<double> xs(beside.begin(), beside.end()), sums(beside.size());
    std::vector<double> rows(b.y1 - b.y0);
    std::array<std::vector<double>, 4> filled;
    double middle = (double(b.x0) + double(b.x1)) / 2;
    for (uint32_t ch = 0; ch < 4; ch++) {
        double total = 0;
        std::fill(rows.begin(), rows.end(), 0.0);
        for (size_t i = 0; i < beside.size(); i++) {
            double s = 0;
            for (uint32_t y = b.y0; y < b.y1; y++) {
                double v = channel(c.at(beside[i], y), ch);
                s += v;
                rows[y - b.y0] += v;
            }
            sums[i] = s;
            total += s;
        }
        std::optional<Quadratic> q = fit_quadratic(xs, sums, middle);
        if (!q) {
            return false;
        }
        filled[ch].resize(size_t(b.x1 - b.x0) * (b.y1 - b.y0));
        for (uint32_t y = b.y0; y < b.y1; y++) {
            double share = total > 0 ? rows[y - b.y0] / total : 0;
            for (uint32_t x = b.x0; x < b.x1; x++) {
                filled[ch][size_t(y - b.y0) * (b.x1 - b.x0) + (x - b.x0)] = std::max(0.0, q->at(x)) * share;
            }
        }
    }
    for (uint32_t y = b.y0; y < b.y1; y++) {
        for (uint32_t x = b.x0; x < b.x1; x++) {
            size_t i = size_t(y - b.y0) * (b.x1 - b.x0) + (x - b.x0);
            uint32_t texel = 0;
            for (uint32_t ch = 0; ch < 4; ch++) {
                texel |= uint32_t(std::clamp(std::lround(filled[ch][i]), 0l, 255l)) << (8 * ch);
            }
            c.at(x, y) = texel;
        }
    }
    return true;
}

const data::Glyph *glyph_of(char ch) {
    for (const data::Glyph &g : data::glyphs) {
        if (g.character == ch) {
            return &g;
        }
    }
    return nullptr;
}

// A word drawn at the letters' own size (data::cap_height capitals): coverage 0-255, its rows top down, x 0 its ink's
// left, `ascent` rows above the baseline
struct WordImage {
    std::vector<uint8_t> coverage;
    uint32_t width = 0, height = 0, ascent = 0;
};

WordImage word_image(std::string_view text) {
    int32_t pen = 0, left = INT32_MAX, right = INT32_MIN, ascent = 0, descent = 0;
    for (char ch : text) {
        const data::Glyph *g = glyph_of(ch);
        if (!g) {
            continue;
        }
        if (g->width > 0) {
            left = std::min(left, pen + g->left);
            right = std::max(right, pen + g->left + int32_t(g->width));
            ascent = std::max(ascent, int32_t(g->top));
            descent = std::max(descent, int32_t(g->height) - g->top);
        }
        pen += g->advance;
    }
    WordImage w;
    if (left >= right) {
        return w;
    }
    w.width = uint32_t(right - left);
    w.height = uint32_t(ascent + descent);
    w.ascent = uint32_t(ascent);
    w.coverage.assign(size_t(w.width) * w.height, 0);
    pen = 0;
    for (char ch : text) {
        const data::Glyph *g = glyph_of(ch);
        if (!g) {
            continue;
        }
        uint32_t x0 = uint32_t(pen + g->left - left), y0 = uint32_t(ascent - g->top);
        for (uint32_t y = 0; y < g->height; y++) {
            for (uint32_t x = 0; x < g->width; x++) {
                uint8_t &out = w.coverage[size_t(y0 + y) * w.width + x0 + x];
                out = std::max(out, data::coverage[g->start + y * g->width + x]);
            }
        }
        pen += g->advance;
    }
    return w;
}

// How much of source cell j lies under output cell i, for output cells `size` source cells wide starting at
// `origin` (in output cells): the 1-D part of an area average
double overlap(int32_t i, uint32_t j, double origin, double size) {
    double a = (double(i) - origin) / size, b = (double(i) + 1 - origin) / size;
    double lo = std::max(a, double(j)), hi = std::min(b, double(j) + 1);
    return hi > lo ? (hi - lo) * size : 0.0;
}

// Draws a word into the finer picture: its letters `sx` x `sy` finer texels to a letter's, its ink's left at finer
// column `left` and its baseline under finer row `baseline`, each finer texel moved toward `colour` by how much of it
// the letters cover (their area averaged over it)
void draw_word(const Canvas &fine, const WordImage &w, double left, double baseline, double sx, double sy,
               uint32_t colour) {
    double top = baseline - double(w.ascent) * sy;
    int32_t fx0 = int32_t(std::floor(left)), fx1 = int32_t(std::ceil(left + w.width * sx));
    int32_t fy0 = int32_t(std::floor(top)), fy1 = int32_t(std::ceil(top + w.height * sy));
    // (each output column's coverage of the word's columns, then of its rows)
    std::vector<double> rows_done(size_t(fx1 - fx0) * w.height, 0.0);
    for (uint32_t y = 0; y < w.height; y++) {
        for (int32_t fx = fx0; fx < fx1; fx++) {
            double sum = 0;
            uint32_t j0 = uint32_t(std::max(0.0, std::floor((fx - left) / sx)));
            uint32_t j1 = std::min(w.width, uint32_t(std::max(0.0, std::ceil((fx + 1 - left) / sx))) + 1);
            for (uint32_t j = j0; j < j1; j++) {
                sum += overlap(fx, j, left, sx) * w.coverage[size_t(y) * w.width + j];
            }
            rows_done[size_t(y) * (fx1 - fx0) + (fx - fx0)] = sum;
        }
    }
    for (int32_t fy = std::max(fy0, 0); fy < std::min(fy1, int32_t(fine.height)); fy++) {
        uint32_t i0 = uint32_t(std::max(0.0, std::floor((fy - top) / sy)));
        uint32_t i1 = std::min(w.height, uint32_t(std::max(0.0, std::ceil((fy + 1 - top) / sy))) + 1);
        for (int32_t fx = std::max(fx0, 0); fx < std::min(fx1, int32_t(fine.width)); fx++) {
            double sum = 0;
            for (uint32_t i = i0; i < i1; i++) {
                sum += overlap(fy, i, top, sy) * rows_done[size_t(i) * (fx1 - fx0) + (fx - fx0)];
            }
            double cover = std::clamp(sum / 255.0, 0.0, 1.0);
            if (cover <= 0) {
                continue;
            }
            uint32_t &texel = fine.at(uint32_t(fx), uint32_t(fy));
            uint32_t out = 0;
            for (uint32_t ch = 0; ch < 4; ch++) {
                double v = channel(texel, ch) * (1 - cover) + channel(colour, ch) * cover;
                out |= uint32_t(std::clamp(std::lround(v), 0l, 255l)) << (8 * ch);
            }
            texel = out;
        }
    }
}

// A word drawn again where the game's was: its capitals from cap_top to the baseline, narrowed to fit its region
void draw_new_word(const Canvas &fine, const data::Word &w, const OldWord &old) {
    WordImage image = word_image(w.text);
    if (image.width == 0) {
        return;
    }
    double s = double(w.baseline - w.cap_top) * k / data::cap_height;
    double room = double(w.x1 - w.x0) * k;
    double sx = std::min(s, room / image.width);
    double width = image.width * sx;
    double left = w.align == data::Align::Left ? double(old.ink.x0) * k
                                               : double(old.ink.x0 + old.ink.x1) * k / 2 - width / 2;
    left = std::clamp(left, double(w.x0) * k, std::max(double(w.x0) * k, double(w.x1) * k - width));
    draw_word(fine, image, std::round(left), double(w.baseline) * k, sx, s, old.colour);
}

std::vector<uint32_t> redrawn(const data::Picture &p, std::span<const uint32_t> colours) {
    std::vector<uint32_t> base(colours.begin(), colours.end());
    Canvas coarse = {base, p.width, p.height};
    std::vector<std::optional<OldWord>> old(p.word_count);
    for (uint32_t n = 0; n < p.word_count; n++) {
        const data::Word &w = data::words[p.first_word + n];
        std::optional<OldWord> found = find_old_word(coarse, w);
        if (!found) {
            continue;
        }
        Box b = painted_box(coarse, w, found->ink);
        if (p.background == data::Background::Plain || !paint_glow(coarse, b)) {
            paint_plain(coarse, b);
        }
        old[n] = found;
    }
    std::vector<uint32_t> fine_colours = enlarged_as_read(base, p.width, p.height, k);
    Canvas fine = {fine_colours, p.width * k, p.height * k};
    for (uint32_t n = 0; n < p.word_count; n++) {
        const std::optional<OldWord> &found = old[n];
        if (found) {
            draw_new_word(fine, data::words[p.first_word + n], *found);
        }
    }
    return fine_colours;
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

bool may_be_text_picture(const DecodedTexture &texture) {
    if (texture.kind != TexelKind::Index8 || texture.levels != 1 ||
        texture.indices.size() < size_t(texture.width_px) * texture.height_px) {
        return false;
    }
    return std::any_of(std::begin(data::pictures), std::end(data::pictures), [&](const data::Picture &p) {
        return p.width == texture.width_px && p.height == texture.height_px;
    });
}

std::vector<uint32_t> sharper_text_picture(const DecodedTexture &texture, std::span<const uint32_t> colours) {
    if (!may_be_text_picture(texture) || colours.size() != size_t(texture.width_px) * texture.height_px) {
        return {};
    }
    uint64_t h = indices_hash(texture);
    for (const data::Picture &p : data::pictures) {
        if (p.width == texture.width_px && p.height == texture.height_px && p.hash == h) {
            return redrawn(p, colours);
        }
    }
    return {};
}

std::vector<uint32_t> redrawn_text_picture(size_t picture, std::span<const uint32_t> colours, uint32_t width,
                                           uint32_t height) {
    if (picture >= std::size(data::pictures)) {
        return {};
    }
    const data::Picture &p = data::pictures[picture];
    if (p.width != width || p.height != height || colours.size() != size_t(width) * height) {
        return {};
    }
    return redrawn(p, colours);
}

} // namespace pcr
