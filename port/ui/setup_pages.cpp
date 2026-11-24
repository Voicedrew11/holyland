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
    own(SetupOwn::card_ps2), own(SetupOwn::card_balanced), own(SetupOwn::card_modern), own(SetupOwn::card_keep),
    own(SetupOwn::skip),     own(SetupOwn::cancel),
};

const SetupRowRef controls_rows[] = {
    own(SetupOwn::device),
    {"controls", "Twin-Stick Controls", pad},
    {"controls", "Mouse Sensitivity", keyboard},
    {"controls", "Invert Y Axis", keyboard},
    own(SetupOwn::next),
};

const SetupRowRef picture_rows[] = {
    {"display", "Frame Rate"},     {"graphics", "Widescreen"}, {"graphics", "HUD Position"},
    {"graphics", "Anti-Aliasing"}, {"graphics", "Font"},       own(SetupOwn::next),
};

const SetupRowRef play_rows[] = {
    {"gameplay", "Attack Tracking"}, {"gameplay", "Door Speed"},    {"gameplay", "Camera Jolt on Hit"},
    {"gameplay", "Auto-Loot Gold"},  {"gameplay", "Instant Smith"}, own(SetupOwn::next),
};

const SetupRowRef hands_rows[] = {
    {"gameplay", "Quick Slots"}, {"gameplay", "HUD Size"},   {"gameplay", "HUD Position"},
    {"gameplay", "HUD Layout"},  {"gameplay", "Card Shape"}, own(SetupOwn::next),
};

SetupRowRef emulator_save(int item) {
    SetupRowRef ref = own(SetupOwn::emulator_save);
    ref.item = item;
    return ref;
}

const SetupRowRef emulator_rows[] = {
    emulator_save(0),           emulator_save(1),
    emulator_save(2),           emulator_save(3),
    emulator_save(4),           emulator_save(5),
    own(SetupOwn::emulator_none), own(SetupOwn::emulator_browse),
    own(SetupOwn::next),
};
static_assert(SETUP_EMULATOR_SAVES == 6, "a row for each");

const SetupRowRef ready_rows[] = {own(SetupOwn::finish)};

const SetupPage pages[] = {
    {"welcome", "Welcome", "Choose a preset. You can change individual settings on the next pages.", welcome_rows,
     PreviewKind::none},
    {"controls", "Controls",
     "Keyboard, mouse and controller always work. The input device only filters the settings shown.", controls_rows,
     PreviewKind::controls},
    {"picture", "Display", "Changes preview live in the background.", picture_rows, PreviewKind::screen},
    {"play", "Gameplay", "1x speeds match the original game.", play_rows, PreviewKind::none},
    {"hands", "Quick Slots", "Swap weapons and spells and use items without opening the menu.", hands_rows,
     PreviewKind::hands},
    {"saves", "Emulator Saves",
     "Import your King's Field IV saves from PCSX2 or RetroArch. Holyland saves are never replaced.",
     emulator_rows, PreviewKind::none},
    {"ready", "Finish", "Press F1, or L3 + R3 on a controller, to open settings at any time.", ready_rows,
     PreviewKind::summary},
};

// ---- The presets: Original, Balanced, Modern ----------------------------------------------------------------------
const PresetValue presets[] = {
    {"display", "Frame Rate", {30, PORT_FRAME_RATE_MONITOR, PORT_FRAME_RATE_MONITOR}},
    {"graphics", "Widescreen", {0, 0, 1}},
    {"graphics", "Anti-Aliasing", {PORT_AA_FXAA, PORT_AA_SMAA, PORT_AA_SMAA_T2X}},
    {"graphics", "Font", {0, 1, 1}}, // (gsn.h's GSN_FONT_: the PS2's, IPA Mincho)
    {"graphics", "Glyphs", {0, PORT_GLYPHS_AUTO, PORT_GLYPHS_AUTO}}, // (the disc's; what the player uses, sharp)
    {"controls", "Twin-Stick Controls", {0, 1, 1}},
    {"gameplay", "Attack Tracking", {100, PORT_ATTACK_TRACKING_AUTO, PORT_ATTACK_TRACKING_AUTO}},
    {"gameplay", "Instant Smith", {0, 0, 0}}, // (it changes the game's balance: the player opts in)
    {"gameplay", "Door Speed", {1, 1, 1}},    // (the PS2's: the player may change it on Gameplay)
    {"gameplay", "Auto-Loot Gold", {0, 0, 1}},
    {"gameplay", "Quick Slots", {0, 0, 1}},
};

// ---- Every player's, never asked ----------------------------------------------------------------------------------
const GrantedValue granted[] = {
    {"display", "Fullscreen", 1, false},         // (at the screen's own size)
    {"display", "Vertical Sync", 1, false},      // (On)
    {"display", "Scaling Filter", PORT_SCALING_BEST, false},
    {"display", "Aspect Ratio", ASPECT_4_3, false},
    {"graphics", "Internal Resolution", 0, false}, // (Auto: from the screen's height)
    {"graphics", "Anisotropic Filtering", 16, false},
    {"graphics", "Texture LOD", 1, false}, // (Auto)
    {"graphics", "Deinterlace", 1, false},
    {"graphics", "Progressive Scan", 1, false},
    {"graphics", "Interlace Comb Removal", 1, false},
    {"graphics", "Seamless Texture Scrolling", 1, false},
    {"graphics", "Flicker Filter", 0, false},
    {"graphics", "Depth", PORT_SCENE_DEPTH_GPU, true}, // (never asked: the only one ready for players)
};

// ---- The setup's own rows -----------------------------------------------------------------------------------------
const char ps2_text[] = "30 FPS, 4:3, original controls and font. Resolution matches your display.";
const char balanced_text[] = "Refresh-rate FPS, 4:3, twin-stick controls, sharp font and SMAA.";
const char modern_text[] = "Balanced with 16:9, quick slots, auto-loot gold and SMAA T2x.";
const Choice devices[] = {{"Keyboard & Mouse", 0}, {"Controller", 1}};

struct OwnRow {
    SetupOwn own;
    SettingRow row;
};
const OwnRow own_rows[] = {
    {SetupOwn::none, {"", "", RowKind::heading}},
    {SetupOwn::card_ps2, {"Original", ps2_text, RowKind::action}},
    {SetupOwn::card_balanced, {"Balanced", balanced_text, RowKind::action}},
    {SetupOwn::card_modern, {"Modern", modern_text, RowKind::action}},
    {SetupOwn::card_keep, {"Current Settings", "Start from your current settings.", RowKind::action}},
    {SetupOwn::skip,
     {"Skip", "Start the game with display settings for this monitor. Other settings stay as they are.",
      RowKind::action}},
    {SetupOwn::cancel, {"Cancel", "Discard changes and restore your previous settings.", RowKind::action}},
    {SetupOwn::device,
     {"Input Device", "Filters the settings shown. All devices work at any time.", RowKind::choice, devices}},
    {SetupOwn::next, {"Next", "", RowKind::action}},
    {SetupOwn::finish, {"Finish", "Apply these settings and start the game.", RowKind::action}},
    {SetupOwn::emulator_save, {"", "", RowKind::action}}, // (each save's own row: SaveImports::row)
    {SetupOwn::emulator_none, {"No PCSX2 or RetroArch Saves Found", "", RowKind::heading}},
    {SetupOwn::emulator_browse,
     {"My Save Is in a Different Location",
      "Opens a file chooser to pick a memory card (.ps2) from PCSX2, RetroArch or another PS2 emulator.",
      RowKind::action}},
};

void problem(std::string &out, const char *tab_id, const char *label, const char *what) {
    char line[160];
    std::snprintf(line, sizeof(line), "%s%s / %s: %s", out.empty() ? "" : "; ", tab_id, label, what);
    out += line;
}

// The value is one the row can hold: one of its choices, or in its range
bool holds(const SettingRow &row, int value) {
    if (row.kind == RowKind::number) {
        return value >= row.range.min && value <= row_range_max(row);
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
        return "Original";
    case Preset::balanced:
        return "Balanced";
    case Preset::modern:
        return "Modern";
    }
    return "";
}

const char *preset_text(Preset preset) {
    switch (preset) {
    case Preset::ps2:
        return ps2_text;
    case Preset::balanced:
        return balanced_text;
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
