// A controller's buttons as the player menu's pictures (port/ui/pad_glyphs.hpp): each SDL controller type finds its
// family; each family's face buttons are the ones SDL itself says sit there (a Switch's B at the bottom, Cross on a
// PlayStation's); bindings find their buttons and keys none; the RML names the family's font.
#include "pad_glyphs.hpp"

#include <SDL3/SDL_gamepad.h>

#include <cstdio>
#include <string>

extern "C" {
#include "port/controls.h"
}

using namespace ui;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// The picture's code point in the RML ("&#xE04C;"), or 0
unsigned glyph_of(const std::string &rml) {
    size_t at = rml.find("&#x");
    return at == std::string::npos ? 0 : unsigned(std::stoul(rml.substr(at + 3), nullptr, 16));
}

void families() {
    CHECK(pad_family(SDL_GAMEPAD_TYPE_PS3) == PadFamily::playstation);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_PS4) == PadFamily::playstation);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_PS5) == PadFamily::playstation);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO) == PadFamily::nintendo);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) == PadFamily::nintendo);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_XBOX360) == PadFamily::xbox);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_XBOXONE) == PadFamily::xbox);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_STANDARD) == PadFamily::xbox);
    CHECK(pad_family(SDL_GAMEPAD_TYPE_UNKNOWN) == PadFamily::xbox);
}

// Each family's four face buttons, south east west north, against what SDL calls the button there on a controller
// of that family
void face_buttons_match_sdl() {
    struct Face {
        PadButton button;
        SDL_GamepadButton sdl;
    };
    const Face faces[] = {{PadButton::south, SDL_GAMEPAD_BUTTON_SOUTH},
                          {PadButton::east, SDL_GAMEPAD_BUTTON_EAST},
                          {PadButton::west, SDL_GAMEPAD_BUTTON_WEST},
                          {PadButton::north, SDL_GAMEPAD_BUTTON_NORTH}};
    struct Family {
        SDL_GamepadType type;
        PadFamily family;
        // the code point of the picture for each SDL label (A B X Y, Cross Circle Square Triangle), per Kenney's maps
        unsigned (*glyph)(SDL_GamepadButtonLabel);
    };
    const Family list[] = {
        {SDL_GAMEPAD_TYPE_XBOXONE, PadFamily::xbox,
         [](SDL_GamepadButtonLabel l) -> unsigned {
             return l == SDL_GAMEPAD_BUTTON_LABEL_A   ? 0xE005
                    : l == SDL_GAMEPAD_BUTTON_LABEL_B ? 0xE007
                    : l == SDL_GAMEPAD_BUTTON_LABEL_X ? 0xE01F
                    : l == SDL_GAMEPAD_BUTTON_LABEL_Y ? 0xE021
                                                      : 0;
         }},
        {SDL_GAMEPAD_TYPE_PS5, PadFamily::playstation,
         [](SDL_GamepadButtonLabel l) -> unsigned {
             return l == SDL_GAMEPAD_BUTTON_LABEL_CROSS      ? 0xE04C
                    : l == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE   ? 0xE042
                    : l == SDL_GAMEPAD_BUTTON_LABEL_SQUARE   ? 0xE052
                    : l == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ? 0xE054
                                                             : 0;
         }},
        {SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, PadFamily::nintendo,
         [](SDL_GamepadButtonLabel l) -> unsigned {
             return l == SDL_GAMEPAD_BUTTON_LABEL_A   ? 0xE005
                    : l == SDL_GAMEPAD_BUTTON_LABEL_B ? 0xE007
                    : l == SDL_GAMEPAD_BUTTON_LABEL_X ? 0xE019
                    : l == SDL_GAMEPAD_BUTTON_LABEL_Y ? 0xE01B
                                                      : 0;
         }},
    };
    for (const Family &f : list) {
        CHECK(pad_family(f.type) == f.family);
        for (const Face &face : faces) {
            unsigned want = f.glyph(SDL_GetGamepadButtonLabelForType(f.type, face.sdl));
            unsigned got = glyph_of(pad_glyph_rml(f.family, face.button));
            if (want == 0 || want != got) {
                std::printf("  type %d button %d: want U+%X, got U+%X\n", int(f.type), int(face.sdl), want, got);
            }
            CHECK(want != 0 && want == got);
        }
    }
}

void bindings() {
    CHECK(pad_button_of_binding(BINDING_PAD | SDL_GAMEPAD_BUTTON_SOUTH) == PadButton::south);
    CHECK(pad_button_of_binding(BINDING_PAD | SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) == PadButton::left_shoulder);
    CHECK(pad_button_of_binding(BINDING_PAD | SDL_GAMEPAD_BUTTON_DPAD_RIGHT) == PadButton::dpad_right);
    CHECK(pad_button_of_binding(BINDING_TRIGGER | SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) == PadButton::right_trigger);
    CHECK(!pad_button_of_binding(BINDING_TRIGGER | SDL_GAMEPAD_AXIS_LEFTX));
    CHECK(!pad_button_of_binding(BINDING_PAD | SDL_GAMEPAD_BUTTON_GUIDE));
    CHECK(!pad_button_of_binding(BINDING_KEY | 4));
    CHECK(!pad_button_of_binding(BINDING_MOUSE | 1));
    CHECK(!pad_button_of_binding(BINDING_NONE));
}

void rml() {
    CHECK(pad_glyph_rml(PadFamily::playstation, PadButton::left_shoulder) ==
          "<pad class=\"playstation\">&#xE07B;</pad>");
    CHECK(pad_glyph_rml(PadFamily::xbox, PadButton::dpad_vertical) == "<pad class=\"xbox\">&#xE038;</pad>");
    CHECK(pad_glyph_rml(PadFamily::nintendo, PadButton::start) == "<pad class=\"nintendo\">&#xE00F;</pad>");
    // each font's family is its class's, as kf4.rcss names it
    CHECK(std::string(pad_fonts[int(PadFamily::xbox)].family) == "pad-xbox");
    CHECK(std::string(pad_fonts[int(PadFamily::playstation)].family) == "pad-playstation");
    CHECK(std::string(pad_fonts[int(PadFamily::nintendo)].family) == "pad-nintendo");
}

} // namespace

int main() {
    families();
    face_buttons_match_sdl();
    bindings();
    rml();
    if (failures != 0) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("ui_pad_glyphs: all passed\n");
    return 0;
}
