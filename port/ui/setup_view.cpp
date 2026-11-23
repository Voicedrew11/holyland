// The first-start setup's page as RML (port/ui/setup_view.hpp). The previews are boxes the style sheet draws
// (kf4.rcss: preview, monitor, picture, mark, card): a screen of the monitor's shape with the game's picture in it, and
// on it what the page changes, at the places and sizes the settings give.
#include "setup_view.hpp"

#include <cstdio>
#include <span>

#include "rows_rml.hpp"

#include "port/hands.h"

extern "C" {
#include "port/controls.h"
#include "port/platform.h"
#include "port/twin_stick.h"
}

namespace ui {

namespace {

// a box placed in its parent, in percent of it
std::string placed(const char *element, const char *classes, float left, float top, float width, float height,
                   const std::string &inner = {}) {
    char style[160];
    std::snprintf(style, sizeof(style), "left: %.2f%%; top: %.2f%%; width: %.2f%%; height: %.2f%%;", double(left),
                  double(top), double(width), double(height));
    return std::string("<") + element + " class=\"" + classes + "\" style=\"" + style + "\">" + inner + "</" +
           element + ">";
}

std::string caption(const char *text) {
    std::string out = "<caption>";
    rml_escaped(out, text);
    return out + "</caption>";
}

// The game's picture in a 16:9 screen: the whole of it in widescreen, the 4:3 middle else (in percent of the screen)
struct Picture {
    float left, width;
};
Picture picture() {
    return port_options.widescreen ? Picture{0.0f, 100.0f} : Picture{12.5f, 75.0f};
}

// A screen of the monitor's shape, with the picture in it and the marks inside the picture
std::string monitor(const std::string &marks) {
    Picture p = picture();
    return "<monitor>" + placed("picture", "", p.left, 0.0f, p.width, 100.0f, marks) + "</monitor>";
}

// The health and magic gauges' place: the picture's top left, or the 4:3 middle's in widescreen with Centred
std::string gauges() {
    float left = port_options.widescreen && port_options.widescreen_hud_centred ? 12.5f : 3.0f;
    return placed("mark", "gauges", left, 4.0f, 22.0f, 5.0f);
}

std::string screen_preview() {
    const char *text = port_options.widescreen ? "16:9: more of the world at the sides. Menus and messages stay 4:3."
                                               : "4:3, as on the PS2, with black bars at the sides.";
    return "<preview>" + monitor(gauges()) + caption(text) + "</preview>";
}

// The hands: three cards (spell, hotkey, weapon) at the HUD's corner, at its size, in its layout and shape
std::string hands_preview() {
    if (!port_hands.enabled) {
        return "<preview class=\"off\">" + monitor(gauges()) + caption("Off: the game's own controls for equipping.") +
               "</preview>";
    }
    // The game's cards (game/hands_cross.c) are in the HUD's 640 x 448, drawn on a 4:3 picture's shape also in
    // widescreen: each is 70 wide, 80 high tall and 70 square, 3 apart, the middle one 24 lower in the cross.
    // Here in percent of the picture's height and width, which in widescreen is 16 for 9, not 4 for 3; drawn
    // larger than the game's, to be seen in the small screen (the caption says so).
    const float larger = 1.25f;
    float scale = port_hands.scale * larger;
    float picture_aspect = port_options.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
    float across = 100.0f * (4.0f / 3.0f) / 640.0f / picture_aspect * scale; // (a HUD unit across, in percent)
    float down = 100.0f / 448.0f * scale;                                     // (a HUD unit down)
    float card_h = port_hands.cards == PORT_HANDS_CARDS_SQUARE ? 70.0f : 80.0f;
    float width = 70.0f * across;
    float height = card_h * down;
    float gap = 3.0f * across;
    float drop = port_hands.layout == PORT_HANDS_LAYOUT_CROSS ? 24.0f * down : 0.0f;
    float group_w = width * 3.0f + gap * 2.0f, group_h = height + drop;
    bool right = port_hands.corner == PORT_HANDS_BOTTOM_RIGHT || port_hands.corner == PORT_HANDS_TOP_RIGHT;
    bool top = port_hands.corner == PORT_HANDS_TOP_RIGHT || port_hands.corner == PORT_HANDS_TOP_LEFT;
    float left = right ? 100.0f - 3.0f - group_w : 3.0f;
    float y = top ? (port_hands.corner == PORT_HANDS_TOP_LEFT ? 12.0f : 4.0f) : 100.0f - 5.0f - group_h;
    std::string cards;
    for (int i = 0; i < 3; i++) {
        float card_top = y + (i == 1 ? drop : 0.0f);
        cards += placed("card", "", left + float(i) * (width + gap), card_top, width, height);
    }
    return "<preview>" + monitor(gauges() + cards) + caption("Where the hands show: spell, hotkey and weapon. Drawn larger than in the game.") +
           "</preview>";
}

// A key's name from its binding (the first slot), in its box
std::string key_of(int action) {
    char text[32] = "";
    port_controls_binding_label(port_controls_binding(action, 0), text, sizeof(text));
    std::string out = "<key>";
    rml_escaped(out, text[0] != 0 ? text : "-");
    return out + "</key>";
}

void line(std::string &out, const std::string &keys, const char *what) {
    out += "<legend>" + keys + "<what>";
    rml_escaped(out, what);
    out += "</what></legend>";
}

// What the sticks and keys do with the choices on the page
std::string controls_preview(const SetupFlow &flow, PadFamily family) {
    std::string out = "<preview class=\"legend\">";
    bool twin = port_twin_stick_setting() != 0;
    if (flow.device() == SetupDevice::pad) {
        line(out, pad_glyph_rml(family, PadButton::left_stick), twin ? "Walk and step aside" : "Walk and turn");
        line(out, pad_glyph_rml(family, PadButton::right_stick), twin ? "Turn and look" : "Look up and down");
        line(out, pad_glyph_rml(family, PadButton::dpad_vertical) + pad_glyph_rml(family, PadButton::dpad_horizontal),
             port_hands.enabled ? "Swap weapons, spells and hotkeys" : "Walk and turn, as on the PS2");
    } else {
        line(out,
             key_of(ACTION_LEFT_STICK_UP) + key_of(ACTION_LEFT_STICK_LEFT) + key_of(ACTION_LEFT_STICK_DOWN) +
                 key_of(ACTION_LEFT_STICK_RIGHT),
             "Walk and step aside");
        line(out, "<key>Mouse</key>", "Turn and look");
        line(out, key_of(ACTION_RUN), "Run while held");
        line(out, key_of(ACTION_CROSS), "Talk, pick up and open");
    }
    out += caption("Every key and button can be changed in F1's Key Bindings.");
    return out + "</preview>";
}

// Each page's choices, as they are now
std::string summary(const SetupFlow &flow) {
    std::string out = "<preview class=\"summary\">";
    std::span<const SetupPage> pages = setup_pages();
    const SetupRows &rows = flow.rows();
    for (int page = 1; page + 1 < int(pages.size()); page++) {
        out += "<h3>";
        rml_escaped(out, pages[size_t(page)].title);
        out += "</h3>";
        for (int row = 0; row < rows.row_count(page); row++) {
            if (rows.own(page, row) == SetupOwn::next || rows.shape(page, row).kind == RowKind::heading) {
                continue;
            }
            out += "<line><label>";
            rml_escaped(out, rows.row(page, row).label);
            out += "</label><value>";
            rml_escaped(out, rows.value_text(page, row));
            out += "</value></line>";
        }
    }
    return out + "</preview>";
}

std::string preview(const SetupFlow &flow, PadFamily family) {
    switch (setup_pages()[size_t(flow.page())].preview) {
    case PreviewKind::controls:
        return controls_preview(flow, family);
    case PreviewKind::screen:
        return screen_preview();
    case PreviewKind::hands:
        return hands_preview();
    case PreviewKind::summary:
        return summary(flow);
    case PreviewKind::none:
        break;
    }
    return {};
}

} // namespace

std::string setup_steps_rml(const SetupFlow &flow) {
    std::string out;
    std::span<const SetupPage> pages = setup_pages();
    for (int page = 0; page < int(pages.size()); page++) {
        out += "<tab data-page=\"" + std::to_string(page) + "\" class=\"step";
        out += page == flow.page() ? " selected" : "";
        out += page < flow.page() || page <= flow.pages_reached() ? " reached" : "";
        out += "\">";
        rml_escaped(out, pages[size_t(page)].title);
        out += "</tab>";
    }
    return out;
}

std::string setup_heading_rml(const SetupFlow &flow) {
    const SetupPage &page = setup_pages()[size_t(flow.page())];
    std::string out;
    rml_escaped(out, page.title);
    out += "<intro>";
    rml_escaped(out, page.intro);
    return out + "</intro>";
}

std::string setup_rows_rml(const SetupFlow &flow, PadFamily family) {
    return rows_rml(flow.rows(), flow.model(), family);
}

std::string setup_help_rml(const SetupFlow &flow, PadFamily family) {
    return preview(flow, family) + help_rml(flow.rows(), flow.model());
}

} // namespace ui
