// The first-start setup as data (port/ui/setup_pages.hpp). The rows named here are the player menu's
// (port/ui/settings_rows.cpp), by their section's id and their label; setup_problems names any that are not found.
#include "setup_pages.hpp"

#include <cstdio>
#include <iterator>

extern "C" {
#include "port/antialias.h"
#include "port/host.h"
#include "port/platform.h"
#include "port/scene_depth.h"
#include "port/settings.h"
}

namespace ui {

namespace {

using enum SetupDevice;

SetupRowRef own(SetupOwn row) {
    SetupRowRef ref;
    ref.own = row;
    return ref;
}

// ---- The pages ----------------------------------------------------------------------------------------------------
const SetupRowRef welcome_rows[] = {
    own(SetupOwn::card_ps2), own(SetupOwn::card_improved), own(SetupOwn::card_modern), own(SetupOwn::card_keep),
    own(SetupOwn::skip),     own(SetupOwn::cancel),
};

const SetupRowRef controls_rows[] = {
    own(SetupOwn::device),
    {"controls", "Twin-stick controls", pad},
    {"controls", "Mouse sensitivity", keyboard},
    {"controls", "Invert Y axis", keyboard},
    own(SetupOwn::next),
};

const SetupRowRef picture_rows[] = {
    {"display", "Frame rate"},     {"graphics", "Widescreen"}, {"graphics", "HUD position"},
    {"graphics", "Anti-aliasing"}, {"graphics", "Font"},       own(SetupOwn::next),
};

const SetupRowRef play_rows[] = {
    {"gameplay", "Attack tracking"}, {"gameplay", "Door speed"},    {"gameplay", "Camera jolt on hit"},
    {"gameplay", "Auto-loot gold"},  {"gameplay", "Instant smith"}, own(SetupOwn::next),
};

const SetupRowRef hands_rows[] = {
    {"gameplay", "Hands and hotkeys"}, {"gameplay", "HUD size"},   {"gameplay", "HUD position"},
    {"gameplay", "HUD layout"},        {"gameplay", "Card shape"}, own(SetupOwn::next),
};

const SetupRowRef ready_rows[] = {own(SetupOwn::finish)};

const SetupPage pages[] = {
    {"welcome", "Welcome",
     "Pick a starting point. The pages after it show what it chose, and you can change any of it there.",
     welcome_rows, PreviewKind::none},
    {"controls", "Controls",
     "The keyboard, the mouse and a controller all work whatever you pick here. This only chooses which settings "
     "this page shows.",
     controls_rows, PreviewKind::controls},
    {"picture", "Picture", "Each change shows at once on the game behind.", picture_rows, PreviewKind::screen},
    {"play", "How it plays", "1x is the PS2's.", play_rows, PreviewKind::none},
    {"hands", "Hands and hotkeys",
     "Swap weapons and spells, and use items, without opening the menu.", hands_rows, PreviewKind::hands},
    {"ready", "Ready", "F1, or L3 and R3 together on a controller, opens every setting at any time.", ready_rows,
     PreviewKind::summary},
};

// ---- The presets: As on the PS2, Improved, Modern -----------------------------------------------------------------
const PresetValue presets[] = {
    {"display", "Frame rate", {30, PORT_FRAME_RATE_MONITOR, PORT_FRAME_RATE_MONITOR}},
    {"graphics", "Widescreen", {0, 0, 1}},
    {"graphics", "Anti-aliasing", {PORT_AA_FXAA, PORT_AA_SMAA, PORT_AA_SMAA_T2X}},
    {"graphics", "Font", {0, 1, 1}}, // (gsn.h's GSN_FONT_: the PS2's, IPA Mincho)
    {"controls", "Twin-stick controls", {0, 1, 1}},
    {"gameplay", "Attack tracking", {100, PORT_ATTACK_TRACKING_AUTO, PORT_ATTACK_TRACKING_AUTO}},
    {"gameplay", "Instant smith", {0, 0, 0}}, // (it changes the game's balance: the player opts in)
    {"gameplay", "Door speed", {1, 1, 1}},    // (the PS2's: the player may change it on How it plays)
    {"gameplay", "Auto-loot gold", {0, 0, 1}},
    {"gameplay", "Hands and hotkeys", {0, 0, 1}},
};

// ---- Every player's, never asked ----------------------------------------------------------------------------------
const GrantedValue granted[] = {
    {"display", "Fullscreen", 1, false},         // (at the screen's own size)
    {"display", "Vertical sync", 1, false},      // (On)
    {"display", "Scaling filter", PORT_SCALING_BEST, false},
    {"display", "Aspect ratio", ASPECT_4_3, false},
    {"graphics", "Internal resolution", 0, false}, // (Auto: from the screen's height)
    {"graphics", "Anisotropic filtering", 16, false},
    {"graphics", "Texture LOD", 1, false}, // (Auto)
    {"graphics", "Deinterlace", 1, false},
    {"graphics", "Progressive scan", 1, false},
    {"graphics", "Interlace comb removal", 1, false},
    {"graphics", "Seamless texture scrolling", 1, false},
    {"graphics", "Flicker filter", 0, false},
    {"graphics", "Depth", PORT_SCENE_DEPTH_GPU, true}, // (never asked: the only one ready for players)
};

// ---- The setup's own rows -----------------------------------------------------------------------------------------
const char ps2_text[] = "The game as it was: 30 frames a second, 4:3, the PS2's controls and text. Only the "
                        "screen is set up for this monitor.";
const char improved_text[] = "The PS2's look and pace, smoother and sharper: your monitor's frame rate, sharp text "
                             "and twin-stick controls.";
const char modern_text[] = "Improved, with widescreen, the hands and hotkeys, and gold picked up as you go.";
const Choice devices[] = {{"Keyboard and mouse", 0}, {"Controller", 1}};

struct OwnRow {
    SetupOwn own;
    SettingRow row;
};
const OwnRow own_rows[] = {
    {SetupOwn::none, {"", "", RowKind::heading}},
    {SetupOwn::card_ps2, {"As on the PS2", ps2_text, RowKind::action}},
    {SetupOwn::card_improved, {"Improved", improved_text, RowKind::action}},
    {SetupOwn::card_modern, {"Modern", modern_text, RowKind::action}},
    {SetupOwn::card_keep,
     {"Keep my settings", "Your settings as they are now. The pages after this start from them.", RowKind::action}},
    {SetupOwn::skip,
     {"Skip setup",
      "Starts the game with the screen set up for this monitor and everything else as it is. F1 has every setting.",
      RowKind::action}},
    {SetupOwn::cancel, {"Cancel", "Puts back every setting as it was when setup opened.", RowKind::action}},
    {SetupOwn::device, {"I play with", "Which settings this page shows. Both always work.", RowKind::choice, devices}},
    {SetupOwn::next, {"Continue", "", RowKind::action}},
    {SetupOwn::finish, {"Start", "Keeps these settings. F1 changes any of them later.", RowKind::action}},
};

void problem(std::string &out, const char *tab_id, const char *label, const char *what) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s%s / %s: %s", out.empty() ? "" : "; ", tab_id, label, what);
    out += line;
}

// The value is one the row can hold: one of its choices, or in its range
bool holds(const SettingRow &row, int value) {
    if (row.kind == RowKind::number) {
        return value >= row.range.min && value <= row.range.max;
    }
    return row_choice_index(row, value) >= 0;
}

void check_value(std::string &out, RowFinder find, const char *tab_id, const char *label, int value) {
    const SettingRow *row = find(tab_id, label);
    if (row == nullptr) {
        problem(out, tab_id, label, "no such row");
    } else if (row->set == nullptr) {
        problem(out, tab_id, label, "the row sets nothing");
    } else if (!holds(*row, value)) {
        problem(out, tab_id, label, "a value the row cannot hold");
    }
}

} // namespace

std::span<const SetupPage> setup_pages() {
    return pages;
}

std::span<const PresetValue> preset_values() {
    return presets;
}

std::span<const GrantedValue> granted_values() {
    return granted;
}

const char *preset_name(Preset preset) {
    switch (preset) {
    case Preset::ps2:
        return "As on the PS2";
    case Preset::improved:
        return "Improved";
    case Preset::modern:
        return "Modern";
    }
    return "";
}

const char *preset_text(Preset preset) {
    switch (preset) {
    case Preset::ps2:
        return ps2_text;
    case Preset::improved:
        return improved_text;
    case Preset::modern:
        return modern_text;
    }
    return "";
}

const SettingRow &setup_own_row(SetupOwn own) {
    for (const OwnRow &row : own_rows) {
        if (row.own == own) {
            return row.row;
        }
    }
    return own_rows[0].row;
}

std::string setup_problems(RowFinder find) {
    std::string out;
    for (const SetupPage &page : pages) {
        for (const SetupRowRef &ref : page.rows) {
            if (ref.own == SetupOwn::none && find(ref.tab_id, ref.label) == nullptr) {
                problem(out, ref.tab_id, ref.label, "no such row");
            }
        }
    }
    for (const PresetValue &preset : presets) {
        for (int value : preset.value) {
            check_value(out, find, preset.tab_id, preset.label, value);
        }
    }
    for (const GrantedValue &value : granted) {
        check_value(out, find, value.tab_id, value.label, value.value);
    }
    return out;
}

} // namespace ui
