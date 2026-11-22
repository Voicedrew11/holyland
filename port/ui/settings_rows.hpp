// The player menu's settings, as data (port/ui/settings_rows.cpp): its tabs, and in each the rows, each with its
// label, help, kind, choices and how it reads and sets its value. A setting is added by adding its row; the menu
// (port/ui/player_ui.cpp) and its moving about (menu_model.hpp) take any row as it is.
//
// A row sets what the settings overlay set before it (port_settings, port_options, port_live_set ...), so
// settings.json and the command line work as they did. All on the game's thread.
#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "menu_model.hpp"

namespace ui {

struct Choice {
    const char *label;
    int value;
};

struct NumberRange {
    int min, max, step;
};

struct SettingRow {
    const char *label;
    const char *help; // shown beside the rows while this one has the focus
    RowKind kind;
    std::span<const Choice> choices = {}; // choice
    NumberRange range = {};          // number
    int (*get)() = nullptr;          // choice: one of the choices' values; toggle: 0 or 1; number: the value
    void (*set)(int value) = nullptr;
    void (*act)() = nullptr;                                     // action
    // the value's text: a number's always (nullptr: the number); a choice's when it writes one (empty: the label)
    void (*describe)(int value, char *out, size_t size) = nullptr;
    bool (*shown)() = nullptr;                                   // nullptr: always
    bool (*enabled)() = nullptr;                                 // nullptr: always
    bool (*overridden)() = nullptr; // the command line chose this run's value: the row says so
    const char *(*note)() = nullptr; // a line under the help, that changes (the cheats' last message); nullptr: none
    int action = -1;                 // binding: its PortAction (port/include/port/controls.h); shown while it is used
};

struct SettingTab {
    const char *id;   // for KF4_OVERLAY_SHOT's ui-<id>
    const char *name; // the tab's title
    std::span<const SettingRow> rows;
    bool (*shown)() = nullptr; // nullptr: always
};

std::span<const SettingTab> setting_tabs();

// The shown tabs' shown rows (a tab's or a row's shown() can hide it), for the model and the view; a tab's index is
// among the shown tabs
class SettingsRows final : public MenuRows {
public:
    int tab_count() const override;
    int row_count(int tab) const override;
    RowShape shape(int tab, int row) const override;
    void choose(int tab, int row, int choice) override;
    void step(int tab, int row, int delta) override;
    void activate(int tab, int row) override;
    void listen(int tab, int row, int slot) override;
    void clear(int tab, int row, int slot) override;

    const SettingTab &tab(int tab) const;
    const SettingRow &row(int tab, int row) const;
    // the row's value as the menu shows it ("On", "Auto (2x)", "75%"); empty for actions and headings
    std::string value_text(int tab, int row) const;
    // a binding's slot as the menu shows it ("Space", "Pad A", "-", "Press a key...")
    std::string slot_text(int tab, int row, int slot) const;
    // a binding's slot is waiting for a key or button
    bool listening(int tab, int row, int slot) const;
    // a binding is not as it was by default
    bool changed_from_default(int tab, int row) const;
    // a number's place in its range, 0 to 1 (for its bar)
    float number_fraction(int tab, int row) const;
    // the shown tab of that id, or -1
    int tab_of(const char *id) const;
};

} // namespace ui
