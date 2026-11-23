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
#include <string_view>

#include "menu_model.hpp"

namespace ui {

struct Choice {
    const char *label;
    int value;
    bool (*enabled)() = nullptr; // nullptr: always; false: shown dimmed, never chosen from the menu
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

// The row of that label in the tab of that id, headings left out (shown or not), or nullptr: how the first-start
// setup (setup_pages.hpp) names the rows it uses
const SettingRow *find_setting_row(std::string_view tab_id, std::string_view label);

// ---- One row, whatever holds it (the menu's tabs, the setup's pages) ------------------------------------------------
RowShape row_shape(const SettingRow &row);
// the choice's place among the row's choices of a value, or -1
int row_choice_index(const SettingRow &row, int value);
bool row_choice_enabled(const SettingRow &row, int choice);
void row_choose(const SettingRow &row, int choice); // a choice's or a toggle's: sets that choice's value
void row_step(const SettingRow &row, int delta);    // a number's: to the next whole step that way
void row_activate(const SettingRow &row);           // an action's
// the row's value as the menu shows it ("On", "Auto (2x)", "75%"); empty for actions, headings and bindings
std::string row_value_text(const SettingRow &row);
// a number's place in its range, 0 to 1 (for its bar)
float row_number_fraction(const SettingRow &row);

// Rows the view can write (port/ui/rows_rml.hpp): the model's rows, with each one's SettingRow and texts
class RowSource : public MenuRows {
public:
    virtual const SettingRow &row(int tab, int row) const = 0;
    virtual std::string value_text(int tab, int row) const = 0;
    // a binding's slot as the menu shows it ("Space", "Pad A", "-", "Press a key...")
    virtual std::string slot_text(int tab, int row, int slot) const = 0;
    // a binding's slot is waiting for a key or button
    virtual bool listening(int tab, int row, int slot) const = 0;
    // a binding is not as it was by default
    virtual bool changed_from_default(int tab, int row) const = 0;
    virtual float number_fraction(int tab, int row) const = 0;
    // more of the row's classes for the style sheet ("card lit"), or empty
    virtual std::string row_classes(int tab, int row) const {
        (void)tab, (void)row;
        return {};
    }
};

// The shown tabs' shown rows (a tab's or a row's shown() can hide it), for the model and the view; a tab's index is
// among the shown tabs
class SettingsRows final : public RowSource {
public:
    int tab_count() const override;
    int row_count(int tab) const override;
    RowShape shape(int tab, int row) const override;
    void choose(int tab, int row, int choice) override;
    void step(int tab, int row, int delta) override;
    void activate(int tab, int row) override;
    void listen(int tab, int row, int slot) override;
    void clear(int tab, int row, int slot) override;
    bool choice_enabled(int tab, int row, int choice) const override;

    const SettingRow &row(int tab, int row) const override;
    std::string value_text(int tab, int row) const override;
    std::string slot_text(int tab, int row, int slot) const override;
    bool listening(int tab, int row, int slot) const override;
    bool changed_from_default(int tab, int row) const override;
    float number_fraction(int tab, int row) const override;

    const SettingTab &tab(int tab) const;
    // the shown tab of that id, or -1
    int tab_of(const char *id) const;
};

} // namespace ui
