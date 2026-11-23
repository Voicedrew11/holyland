// The first-start setup as data (port/ui/setup_model.hpp moves through it): its pages in order, each a title, a line
// or two under it, the rows it asks (the player menu's own rows, named by their section and label, or the setup's
// own: the Welcome's cards, the device, Continue) and what its preview draws; the presets the Welcome's cards choose;
// and the settings every player gets without being asked. A page, a row or a preset's value is an entry here.
// No RmlUi and no settings code: a row named here is found through a RowFinder (find_setting_row in the game).
#pragma once

#include <span>
#include <string_view>

#include "settings_rows.hpp"

namespace ui {

// The Welcome's cards: each fills every page with its values; the player then walks the pages
enum class Preset { ps2, improved, modern };
inline constexpr int PRESET_COUNT = 3;

// Whose row it is on the Controls page: everyone's, the keyboard and mouse player's, the controller player's
enum class SetupDevice { any, keyboard, pad };

// The setup's own rows (not the player menu's)
enum class SetupOwn {
    none,
    card_ps2, // the Welcome's cards: the preset, then the next page
    card_improved,
    card_modern,
    card_keep, // the player's own settings as they are (only offered when there were some)
    skip,      // the Welcome: only the settings given to everyone, and done
    cancel,    // the Welcome, when run again from the menu: everything put back
    device,    // Controls: what the player plays with (which rows show)
    next,      // the end of each page
    finish,    // the last page's
};

// What the help column draws above the focused row's help
enum class PreviewKind { none, controls, screen, hands, summary };

struct SetupRowRef {
    const char *tab_id = nullptr; // a player menu row: its section's id and its label (find_setting_row)
    const char *label = nullptr;
    SetupDevice device = SetupDevice::any;
    SetupOwn own = SetupOwn::none; // else one of the setup's own rows (tab_id and label unused)
};

struct SetupPage {
    const char *id;    // for KF4_OVERLAY_SHOT's setup-<id>
    const char *title; // the sidebar's and the heading's
    const char *intro; // under the heading
    std::span<const SetupRowRef> rows;
    PreviewKind preview;
};

// A row's value in each preset, in Preset's order
struct PresetValue {
    const char *tab_id;
    const char *label;
    int value[PRESET_COUNT];
};

// A setting every player gets, never asked: with any card, and with Skip. keep: also when the player keeps their
// own settings (the depth)
struct GrantedValue {
    const char *tab_id;
    const char *label;
    int value;
    bool keep;
};

using RowFinder = const SettingRow *(*)(std::string_view tab_id, std::string_view label);

std::span<const SetupPage> setup_pages();
std::span<const PresetValue> preset_values();
std::span<const GrantedValue> granted_values();
// a card's name and the line under it
const char *preset_name(Preset preset);
const char *preset_text(Preset preset);
// the setup's own row (its label, help and kind; its get, set and act unused: the setup does them)
const SettingRow &setup_own_row(SetupOwn own);

// What in the tables does not fit the rows found (a row not found, a preset value that is none of its row's
// choices); empty when all is well. Logged at the game's start.
std::string setup_problems(RowFinder find);

} // namespace ui
