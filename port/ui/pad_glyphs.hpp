// A controller's buttons as pictures for the player menu: the prompts along the bottom, the keys that change the
// section, the controller slot of a key binding. Each family of controllers has its own pictures, as the player's
// controller shows them (Cross and Circle on a PlayStation's, A and B on an Xbox's, B and A on a Switch's), drawn from
// Kenney's Input Prompts fonts (CC0; port/ui/res/fonts/kenney_input_*.ttf, loaded as one font family each).
//
// Buttons are named by where they sit, as SDL names them (south is the bottom face button: Cross, A, or a Switch's B),
// so a binding's SDL button and the menu's own buttons (south chooses, east backs out) find their picture alike.
#pragma once

#include <optional>
#include <string>

namespace ui {

enum class PadFamily {
    xbox,        // Xbox's and the rest (Steam Deck, generic controllers): A B X Y, LB RB
    playstation, // Cross Circle Square Triangle, L1 R1
    nintendo,    // a Switch's: B A Y X where Xbox's A B X Y sit, L R
};

enum class PadButton {
    south,
    east,
    west,
    north,
    back, // Select, Share, View, Minus
    start, // Start, Options, Menu, Plus
    left_stick, // pressed in: L3
    right_stick,
    left_shoulder, // L1, LB, L
    right_shoulder,
    left_trigger, // L2, LT, ZL
    right_trigger,
    dpad_up,
    dpad_down,
    dpad_left,
    dpad_right,
    dpad_vertical, // up and down, as one picture
    dpad_horizontal,
};

// The family of an SDL_GamepadType (SDL_GAMEPAD_TYPE_PS5, ...)
PadFamily pad_family(int sdl_gamepad_type);

// The button a key binding's controller slot holds (port/include/port/controls.h: BINDING_PAD with an SDL button,
// BINDING_TRIGGER with a trigger's axis); nullopt for keys, mouse buttons, the guide button and no binding
std::optional<PadButton> pad_button_of_binding(int binding);

// The button's picture as RML: <pad class="playstation">&#xE04B;</pad>. kf4.rcss gives each family's class its font.
std::string pad_glyph_rml(PadFamily family, PadButton button);

// The fonts the pictures need, for RmlUi to load: each a family named after its class ("pad-playstation")
struct PadFont {
    const char *path;   // under port/ui/res/
    const char *family; // the font family kf4.rcss names
};
extern const PadFont pad_fonts[3];

} // namespace ui
