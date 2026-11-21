// A controller's buttons as pictures for the player menu: pad_glyphs.hpp
#include "pad_glyphs.hpp"

#include <SDL3/SDL_gamepad.h>

#include <cstdio>

extern "C" {
#include "port/controls.h"
}

namespace ui {

namespace {

constexpr int BUTTON_COUNT = int(PadButton::dpad_horizontal) + 1;

// Each family's font and its pictures, by PadButton, as Kenney's maps name them (fonts/kenney_input_*_map.txt): the
// outlined ones, which sit better with the menu's thin lines than the filled
struct FamilyGlyphs {
    const char *css_class;
    char32_t glyph[BUTTON_COUNT];
};

constexpr FamilyGlyphs families[] = {
    // PadFamily::xbox
    {"xbox",
     {
         0xE005, // south: xbox_button_a_outline
         0xE007, // east: xbox_button_b_outline
         0xE01F, // west: xbox_button_x_outline
         0xE021, // north: xbox_button_y_outline
         0xE01D, // back: xbox_button_view_outline
         0xE015, // start: xbox_button_menu_outline
         0xE046, // left_stick: xbox_ls_outline
         0xE04C, // right_stick: xbox_rs_outline
         0xE044, // left_shoulder: xbox_lb_outline
         0xE04A, // right_shoulder: xbox_rb_outline
         0xE048, // left_trigger: xbox_lt_outline
         0xE04E, // right_trigger: xbox_rt_outline
         0xE036, // dpad_up: xbox_dpad_up_outline
         0xE025, // dpad_down: xbox_dpad_down_outline
         0xE029, // dpad_left: xbox_dpad_left_outline
         0xE02C, // dpad_right: xbox_dpad_right_outline
         0xE038, // dpad_vertical: xbox_dpad_vertical_outline
         0xE027, // dpad_horizontal: xbox_dpad_horizontal_outline
     }},
    // PadFamily::playstation
    {"playstation",
     {
         0xE04C, // south: playstation_button_cross_outline
         0xE042, // east: playstation_button_circle_outline
         0xE052, // west: playstation_button_square_outline
         0xE054, // north: playstation_button_triangle_outline
         0xE021, // back: playstation5_button_create_outline
         0xE027, // start: playstation5_button_options_outline
         0xE04E, // left_stick: playstation_button_l3_outline
         0xE050, // right_stick: playstation_button_r3_outline
         0xE07B, // left_shoulder: playstation_trigger_l1_outline
         0xE083, // right_shoulder: playstation_trigger_r1_outline
         0xE07F, // left_trigger: playstation_trigger_l2_outline
         0xE087, // right_trigger: playstation_trigger_r2_outline
         0xE061, // dpad_up: playstation_dpad_up_outline
         0xE058, // dpad_down: playstation_dpad_down_outline
         0xE05C, // dpad_left: playstation_dpad_left_outline
         0xE05F, // dpad_right: playstation_dpad_right_outline
         0xE063, // dpad_vertical: playstation_dpad_vertical_outline
         0xE05A, // dpad_horizontal: playstation_dpad_horizontal_outline
     }},
    // PadFamily::nintendo: B where Xbox's A sits, A where its B, Y where its X, X where its Y
    {"nintendo",
     {
         0xE007, // south: switch_button_b_outline
         0xE005, // east: switch_button_a_outline
         0xE01B, // west: switch_button_y_outline
         0xE019, // north: switch_button_x_outline
         0xE00D, // back: switch_button_minus_outline
         0xE00F, // start: switch_button_plus_outline
         0xE05E, // left_stick: switch_stick_l_press
         0xE066, // right_stick: switch_stick_r_press
         0xE00B, // left_shoulder: switch_button_l_outline
         0xE011, // right_shoulder: switch_button_r_outline
         0xE01D, // left_trigger: switch_button_zl_outline
         0xE01F, // right_trigger: switch_button_zr_outline
         0xE03D, // dpad_up: switch_dpad_up_outline
         0xE034, // dpad_down: switch_dpad_down_outline
         0xE038, // dpad_left: switch_dpad_left_outline
         0xE03B, // dpad_right: switch_dpad_right_outline
         0xE03F, // dpad_vertical: switch_dpad_vertical_outline
         0xE036, // dpad_horizontal: switch_dpad_horizontal_outline
     }},
};

} // namespace

const PadFont pad_fonts[3] = {
    {"fonts/kenney_input_xbox_series.ttf", "pad-xbox"},
    {"fonts/kenney_input_playstation_series.ttf", "pad-playstation"},
    {"fonts/kenney_input_nintendo_switch.ttf", "pad-nintendo"},
};

PadFamily pad_family(int sdl_gamepad_type) {
    switch (sdl_gamepad_type) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
        return PadFamily::playstation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return PadFamily::nintendo;
    default:
        return PadFamily::xbox;
    }
}

std::optional<PadButton> pad_button_of_binding(int binding) {
    int code = BINDING_CODE(binding);
    if (BINDING_KIND(binding) == BINDING_TRIGGER) {
        if (code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            return PadButton::left_trigger;
        }
        if (code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            return PadButton::right_trigger;
        }
        return std::nullopt;
    }
    if (BINDING_KIND(binding) != BINDING_PAD) {
        return std::nullopt;
    }
    switch (code) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return PadButton::south;
    case SDL_GAMEPAD_BUTTON_EAST: return PadButton::east;
    case SDL_GAMEPAD_BUTTON_WEST: return PadButton::west;
    case SDL_GAMEPAD_BUTTON_NORTH: return PadButton::north;
    case SDL_GAMEPAD_BUTTON_BACK: return PadButton::back;
    case SDL_GAMEPAD_BUTTON_START: return PadButton::start;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return PadButton::left_stick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return PadButton::right_stick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return PadButton::left_shoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PadButton::right_shoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return PadButton::dpad_up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return PadButton::dpad_down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return PadButton::dpad_left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return PadButton::dpad_right;
    default: return std::nullopt; // the guide button and the extra ones: no picture
    }
}

std::string pad_glyph_rml(PadFamily family, PadButton button) {
    const FamilyGlyphs &glyphs = families[int(family)];
    char text[64];
    std::snprintf(text, sizeof(text), "<pad class=\"%s\">&#x%X;</pad>", glyphs.css_class,
                  unsigned(glyphs.glyph[int(button)]));
    return text;
}

} // namespace ui
