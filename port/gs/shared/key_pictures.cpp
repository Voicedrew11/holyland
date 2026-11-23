// The keys and the mouse's buttons as pictures (key_pictures.h): each a shape standing on the board (a height above it
// at each point, a light legend on it), lit as black plastic from one light, its shadow under it.
#include "key_pictures.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <utility>

#include "key_letters.h"

namespace pcr {

// The things a key or a mouse is drawn as: a shape on the board, in texels from its picture's corner
class KeyDrawing::Shape {
public:
    virtual ~Shape() = default;
    // How far outside its outline a point is (negative: inside)
    virtual float outside(float x, float y) const = 0;
    // How high it stands at a point inside it, texels above the board
    virtual float height(float x, float y) const = 0;
    // How much of the light legend colour a point inside it has, 0 (the plastic) to 1
    virtual float legend(float x, float y) const = 0;
    // How high it stands at most
    virtual float tallest() const = 0;
};

namespace {

using Shape = KeyDrawing::Shape;

constexpr uint32_t draw_samples = 4; // (across a finer texel, each way)

uint32_t byte(double v) {
    return uint32_t(std::clamp(std::lround(v), 0l, 255l));
}

struct RoundedBox {
    float x0, y0, x1, y1, radius;

    // How far outside its edge a point is (negative: inside)
    float distance(float x, float y) const {
        float dx = x - std::clamp(x, x0 + radius, x1 - radius), dy = y - std::clamp(y, y0 + radius, y1 - radius);
        return std::sqrt(dx * dx + dy * dy) - radius;
    }
};

// A key one U wide (a letter key's) is a U square with a gap round it; each U more adds a U, as the keys of a real
// keyboard are spaced
constexpr float key_gap = 3.0f; // (between two keys side by side)
constexpr float key_height = key_unit - key_gap;
// How much of its sides shows (left and right, back, front: seen a little from the front) and how tall it stands
constexpr float key_side = 1.9f, key_back = 0.8f, key_front = 3.6f, key_tall = 2.6f;
constexpr float key_corner = 1.7f, key_top_corner = 1.5f;
constexpr float key_dish = 0.22f; // (how deep its top is scooped, side to side)
constexpr float shadow_dy = 0.7f, shadow_soft = 1.4f, shadow_alpha = 0.6f;
constexpr float key_label_margin = 1.6f, key_label_height = 7.0f; // (the most the label is)

// The plastic and the light, in linear light. Seen from the front and a little above; lit from above, behind and to
// the left, as a room light is.
struct Material {
    double plastic, legend;  // (how much light each gives back, of what falls on it)
    double ambient;          // (light from everywhere, against the light's 1)
    double gloss, sheen;     // (a sharp highlight's and a broad one's strength)
    double gloss_power, sheen_power;
};
constexpr Material material = {0.012, 0.62, 0.45, 0.22, 0.012, 70.0, 6.0};
constexpr std::array<double, 3> light_from = {-0.38, -0.62, 0.69}; // (x right, y down the picture, z toward the eye)
constexpr std::array<double, 3> eye_at = {0.0, 0.42, 0.91};
constexpr std::array<double, 3> tint = {1.0, 1.0, 1.04}; // (the plastic's: a little cool)

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

// A shape's colour at a point inside it, linear
std::array<double, 3> lit(const Shape &shape, float x, float y) {
    constexpr float e = 0.12f;
    const std::array<double, 3> normal = normalised({-(shape.height(x + e, y) - shape.height(x - e, y)) / (2 * e),
                                                     -(shape.height(x, y + e) - shape.height(x, y - e)) / (2 * e), 1.0});
    const std::array<double, 3> light = normalised(light_from), eye = normalised(eye_at);
    const std::array<double, 3> half = normalised({light[0] + eye[0], light[1] + eye[1], light[2] + eye[2]});
    const double ink = shape.legend(x, y);
    const double albedo = material.plastic + (material.legend - material.plastic) * ink;
    const double facing = std::max(dot(normal, light), 0.0), towards_half = std::max(dot(normal, half), 0.0);
    // (darker where it meets the board: the light it lets past)
    const double occlusion = 0.45 + 0.55 * std::clamp(double(shape.height(x, y)) / (0.6 * shape.tallest()), 0.0, 1.0);
    const double diffuse = albedo * (material.ambient * (0.6 + 0.4 * normal[2]) + facing) * occlusion;
    const double shine = (material.gloss * (1 - ink * 0.7) * std::pow(towards_half, material.gloss_power) +
                          material.sheen * std::pow(towards_half, material.sheen_power)) *
                         occlusion;
    return {diffuse * tint[0] + shine, diffuse * tint[1] + shine, diffuse * tint[2] + shine};
}

// A key: its body (where it meets the board), its top face and its label
class Key final : public Shape {
public:
    Key(std::string_view text, float width_u)
        : body_{key_gap / 2, key_gap / 2, key_gap / 2 + key_unit * width_u - key_gap, key_gap / 2 + key_height,
                key_corner},
          top_{body_.x0 + key_side, body_.y0 + key_back, body_.x1 - key_side, body_.y1 - key_front, key_top_corner},
          label_(fitted_label(text, top_)) {}

    float outside(float x, float y) const override {
        return body_.distance(x, y);
    }
    float height(float x, float y) const override {
        float outside_top = top_.distance(x, y);
        if (outside_top <= 0) {
            float across = (2 * x - top_.x0 - top_.x1) / (top_.x1 - top_.x0);
            return key_tall - key_dish * (1 - across * across);
        }
        float inside_body = std::max(-body_.distance(x, y), 0.0f);
        float s = inside_body / (inside_body + outside_top); // (0 at the board, 1 at the top face's edge)
        // (steep near the board, rounding over into the top)
        return key_tall * float(1 - std::pow(1 - s, 2.4));
    }
    float legend(float x, float y) const override {
        return top_.distance(x, y) <= 0 ? label_.ink(x, y) : 0.0f;
    }
    float tallest() const override {
        return key_tall;
    }

private:
    static LetterText fitted_label(std::string_view text, const RoundedBox &top) {
        float height = key_label_height, room = top.x1 - top.x0 - 2 * key_label_margin;
        if (float unit_width = LetterText::width_for(key_font(), text, 1); unit_width > 0) {
            height = std::min(height, room / unit_width);
        }
        return LetterText(key_font(), text, (top.x0 + top.x1) / 2, (top.y0 + top.y1) / 2, height);
    }

    RoundedBox body_, top_;
    LetterText label_;
};

// A mouse seen from above, its front up, a U square: its two buttons split by a groove, the wheel between them and
// two buttons on its left side; the button bound lit in the legend's colour
enum class MouseButton { none, left, middle, right, back, forward };

constexpr float mouse_x = key_unit / 2, mouse_y = key_unit / 2 - 0.3f; // (its middle)
constexpr float mouse_half_width = 5.9f, mouse_half_length = 9.4f;
constexpr float mouse_tall = 2.8f;
constexpr float mouse_squareness = 2.7f; // (its outline's power: 2 an ellipse, higher squarer)
constexpr float mouse_split = mouse_y - 0.2f * mouse_half_length; // (where the buttons end, down it)
constexpr float groove_width = 0.35f, groove_depth = 0.45f;

class Mouse final : public Shape {
public:
    explicit Mouse(MouseButton lit) : lit_(lit) {}

    float outside(float x, float y) const override {
        return (reach(x, y) - 1.0f) * mouse_half_width;
    }
    float height(float x, float y) const override {
        float r = std::min(reach(x, y), 1.0f);
        // (a low dome, rounding steeply down at its edge; highest behind its middle, where the palm lies)
        float back = 0.85f + 0.15f * std::clamp((y - mouse_y) / mouse_half_length + 0.3f, 0.0f, 1.0f);
        float h = mouse_tall * back * std::pow(1.0f - std::pow(r, 4.0f), 0.45f);
        // (the grooves round the buttons: down the middle in front, across where they end)
        if (y < mouse_split) {
            h -= groove(x - mouse_x);
        }
        h -= groove(y - mouse_split) * (std::abs(x - mouse_x) < mouse_half_width ? 1.0f : 0.0f);
        if (on_wheel(x, y)) {
            h += 0.25f;
        }
        return std::max(h, 0.0f);
    }
    float legend(float x, float y) const override {
        const float side = mouse_half_width * width_at(y) * 0.74f;
        switch (lit_) {
        case MouseButton::left: return y < mouse_split - groove_width && x < mouse_x - groove_width && !on_wheel(x, y);
        case MouseButton::right: return y < mouse_split - groove_width && x > mouse_x + groove_width && !on_wheel(x, y);
        case MouseButton::middle: return on_wheel(x, y);
        case MouseButton::back: return x < mouse_x - side && y > mouse_y + 0.4f && y < mouse_y + 3.4f;
        case MouseButton::forward: return x < mouse_x - side && y > mouse_y - 3.0f && y < mouse_y - 0.2f;
        case MouseButton::none: return 0.0f;
        }
        return 0.0f;
    }
    float tallest() const override {
        return mouse_tall;
    }

private:
    // (a little narrower at its front than its back)
    static float width_at(float y) {
        return 1.0f + 0.1f * std::clamp((y - mouse_y) / mouse_half_length, -1.0f, 1.0f);
    }
    // How far out a point is, its outline 1
    static float reach(float x, float y) {
        float dx = std::abs(x - mouse_x) / (mouse_half_width * width_at(y)), dy = std::abs(y - mouse_y) / mouse_half_length;
        return std::pow(std::pow(dx, mouse_squareness) + std::pow(dy, mouse_squareness), 1.0f / mouse_squareness);
    }
    static float groove(float d) {
        return groove_depth * std::exp(-(d * d) / (groove_width * groove_width));
    }
    static bool on_wheel(float x, float y) {
        return std::abs(x - mouse_x) < 1.0f && y > mouse_y - 0.66f * mouse_half_length && y < mouse_y - 0.3f * mouse_half_length;
    }

    MouseButton lit_;
};

// The mouse button a binding's name is, if it is one
bool mouse_button_of(std::string_view key_name, MouseButton &out) {
    static constexpr std::array<std::pair<std::string_view, MouseButton>, 6> named = {{
        {"Mouse", MouseButton::none},        {"Mouse left", MouseButton::left},   {"Mouse middle", MouseButton::middle}, {"Mouse right", MouseButton::right},
        {"Mouse 4", MouseButton::back},      {"Mouse 5", MouseButton::forward},
    }};
    for (const auto &[name, button] : named) {
        if (name == key_name) {
            out = button;
            return true;
        }
    }
    return false;
}

std::unique_ptr<Shape> shape_of(std::string_view key_name, float width_u) {
    MouseButton button;
    if (mouse_button_of(key_name, button)) {
        return std::make_unique<Mouse>(button);
    }
    return std::make_unique<Key>(key_label_text(key_name), width_u);
}

} // namespace

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


KeyDrawing::KeyDrawing(std::string_view key_name) : KeyDrawing(key_name, key_width_u(key_name)) {}

KeyDrawing::KeyDrawing(std::string_view key_name, float width_u)
    : shape_(shape_of(key_name, width_u)), width_u_(width_u) {}

KeyDrawing::~KeyDrawing() = default;
KeyDrawing::KeyDrawing(KeyDrawing &&) noexcept = default;
KeyDrawing &KeyDrawing::operator=(KeyDrawing &&) noexcept = default;

float KeyDrawing::width_u() const {
    return width_u_;
}

uint32_t KeyDrawing::texel(float x, float y, float width, float height, double &share) const {
    std::array<double, 3> sum{};
    double cover = 0, shadow = 0;
    for (uint32_t sy = 0; sy < draw_samples; sy++) {
        for (uint32_t sx = 0; sx < draw_samples; sx++) {
            float px = x + width * (sx + 0.5f) / draw_samples, py = y + height * (sy + 0.5f) / draw_samples;
            if (shape_->outside(px, py) <= 0) {
                std::array<double, 3> c = lit(*shape_, px, py);
                for (uint32_t i = 0; i < 3; i++) {
                    sum[i] += c[i];
                }
                cover++;
                continue;
            }
            float fade = std::clamp(1.0f - shape_->outside(px, py - shadow_dy) / shadow_soft, 0.0f, 1.0f);
            shadow += shadow_alpha * fade * fade;
        }
    }
    share = (cover + shadow) / (draw_samples * draw_samples);
    // (the shadow is black; beyond the shape and its shadow the colour is the plastic's, for a filter to blend toward)
    uint32_t out = 0;
    for (uint32_t i = 0; i < 3; i++) {
        out |= srgb_byte(cover + shadow > 0 ? sum[i] / (cover + shadow) : material.plastic) << (8 * i);
    }
    return out;
}

KeyPicture key_picture(std::string_view key_name, uint32_t scale) {
    const KeyDrawing key(key_name);
    KeyPicture out;
    out.width = uint32_t(std::ceil(key.width_u() * key_unit)) * scale;
    out.height = uint32_t(key_unit) * scale;
    out.texels.resize(size_t(out.width) * out.height);
    const float step = 1.0f / float(scale);
    for (uint32_t y = 0; y < out.height; y++) {
        for (uint32_t x = 0; x < out.width; x++) {
            double share = 0;
            uint32_t colour = key.texel(float(x) * step, float(y) * step, step, step, share);
            out.texels[size_t(y) * out.width + x] = colour | byte(255 * share) << 24;
        }
    }
    return out;
}

} // namespace pcr
