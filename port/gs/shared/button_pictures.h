// The controller's buttons the menus show, drawn again sharper, in the style the player chose (the player menu's
// Glyphs): the cross, circle, square and triangle buttons (a symbol on a dark round body, 21 texels across) on the
// sheet of buttons (menu/menu.tmx block 12) and beside the menus' hints (block 1, the hints' sheet: text_pictures.h
// redraws its words). Each picture is known by its indices (button_pictures_data.h, made by
// tools/make_button_pictures.py), which lists its buttons; each is drawn again into the picture made
// button_picture_scale times as fine:
// - PlayStation: its body's shading fitted to the game's own texels round the symbol, its symbol from the shapes
//   measured on the disc (bars, a ring, an outlined square or triangle) with sharp edges, in the symbol's colour;
// - Xbox: the same body, the Xbox controller's letter for the button in the same place (A for cross, B circle,
//   X square, Y triangle: SDL's south, east, west and north) in that letter's colour;
// - Keyboard: the same body, labelled with the key (or mouse button) the button is bound to.
// The other buttons (the d-pad's, the shoulders') are left as the disc has them. A new style is a new entry in
// button_pictures.cpp's table.
//
// One thread at a time: nothing kept between calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pcr_textures.h"

namespace pcr {

// How many times as fine across and down a picture with buttons is drawn
constexpr uint32_t button_picture_scale = 4;

// How the buttons are drawn (gsn.h's GSN_GLYPHS_*, the same numbers): as the disc has them (none drawn again), or one
// of the styles above
enum class ButtonStyle : uint8_t { disc, playstation, xbox, keyboard };

// The face buttons, in the order the keys of ButtonLooks are
enum class FaceButton : uint8_t { cross, circle, square, triangle };
constexpr size_t face_button_count = 4;

// The buttons asked for: their style and, for the keyboard's, what each face button is bound to
struct ButtonLooks {
    ButtonStyle style = ButtonStyle::disc;
    // (the key's or mouse button's name as the controls name it, port_controls_binding_label's: "F", "Escape",
    // "Mouse left"; empty: unbound, the button left blank)
    std::array<std::string, face_button_count> keys;

    bool operator==(const ButtonLooks &) const = default;
    // Whether any button is drawn again
    bool redrawn() const { return style != ButtonStyle::disc; }
    // A hash of all of it, for a texture's key (0 for the disc's)
    uint64_t hash() const;
};

// What the keyboard's button says for a key's name: capitals, five letters at most ("Escape" ESC, "Left Shift" SHIFT,
// "Up" an arrow, "Mouse left" LMB, "F" itself)
std::string key_label_text(std::string_view key_name);

// Whether a decoded texture has the shape of a picture with buttons (one level of indices, a size one of them has)
bool may_have_buttons(const DecodedTexture &texture);

// Whether a texture is one of the disc's pictures with buttons (its indices' hash: the dearer check)
bool has_buttons(const DecodedTexture &texture);

// Draws the texture's buttons again, as looks asks, into fine: the texture's picture made button_picture_scale times as fine already
// (by another kind of finer picture, or enlarged_as_read), colours its own texels (width x height, the sampler's
// RGBA8). False, fine untouched, when the texture is not one of the disc's pictures with buttons or looks asks for
// the disc's.
bool draw_buttons(const DecodedTexture &texture, std::span<const uint32_t> colours, std::span<uint32_t> fine,
                  const ButtonLooks &looks);

// draw_buttons without the check against the disc's pictures (the tests'): the buttons of the table's picture number
// `picture` (button_pictures_data.h's pictures[]) drawn from colours, whatever they hold; false when the sizes are not
// that picture's
bool draw_listed_buttons(size_t picture, std::span<const uint32_t> colours, uint32_t width, uint32_t height,
                         std::span<uint32_t> fine, const ButtonLooks &looks);

} // namespace pcr
