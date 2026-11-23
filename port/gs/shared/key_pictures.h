// The keyboard's keys and the mouse's buttons as pictures, as PC versions of the late 2000s drew their prompts: a black
// key seen a little from the front and above, lit as plastic (sloped sides, a top scooped side to side, a gloss, a
// shadow under it), its label printed light on it in Barlow (key_letters.h); a mouse button as a black mouse with that
// button lit ("Mouse" alone: the mouse, none lit). Keys are as wide as on a real (US) keyboard, in U (a letter key's width): key_width_u.
//
// The menus' buttons draw them (button_pictures.h, the Keyboard style) and so does the player menu (port/ui).
//
// Plain data: any thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pcr {

// A U across, in a picture's texels: as wide and as tall as a button's cell of the disc's button sheets
constexpr float key_unit = 23.0f;

// How wide a key (port_controls_binding_label's name: "Return", "Left Shift", "Mouse left") is, in U: a real
// keyboard's (Tab 1.5, Return 2.25), but Space 3, shortened; 1 for a mouse button
float key_width_u(std::string_view key_name);

// What a key says: capitals, five letters at most ("Escape" ESC, "Left Shift" SHIFT, "Up" an arrow, "F" itself)
std::string key_label_text(std::string_view key_name);

// One key or mouse button drawn a U tall, its corner at a picture's (0, 0): key_width_u wide, or as wide as asked (a
// button's cell is 1 U whatever key it shows)
class KeyDrawing {
public:
    explicit KeyDrawing(std::string_view key_name);
    KeyDrawing(std::string_view key_name, float width_u);
    ~KeyDrawing();
    KeyDrawing(KeyDrawing &&) noexcept;
    KeyDrawing &operator=(KeyDrawing &&) noexcept;

    // How wide it is, in U
    float width_u() const;

    // The colour of the part of the picture from (x, y), width x height texels (sRGB, red in the low byte, no alpha),
    // and in share how much of it the key and its shadow cover (0 to 1: its alpha)
    uint32_t texel(float x, float y, float width, float height, double &share) const;

    // The things a key or a mouse is drawn as (key_pictures.cpp)
    class Shape;

private:
    std::unique_ptr<Shape> shape_;
    float width_u_;
};

// A key drawn on its own: RGBA8, straight alpha (255 opaque), rows top down, key_unit * scale finer texels to a U
struct KeyPicture {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> texels;
};
KeyPicture key_picture(std::string_view key_name, uint32_t scale);

} // namespace pcr
