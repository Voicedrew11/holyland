// The player menu's moving about (port/ui/): which tab and row have the focus, the list of a row's choices when it
// is open, and what each key, button or click does to them. No RmlUi and no settings here: the rows are asked
// through MenuRows (the settings catalogue, port/ui/settings_rows.hpp; tests' stand-ins, tests/ui_menu_test.cpp).
//
//   Up / Down       the row before / after that can be used (not past the ends); in an open list, its choice
//   Left / Right    a choice: the one before / after (not past the ends); on / off: off / on; a number: a step
//   Confirm         a choice: opens its list; on / off: turns it over; an action: does it; in a list: chooses
//   Back            closes the open list, else asks to close the menu
//   Next / previous tab   the tab after / before (round), on its first row that can be used
// The pointer: over a row gives it the focus, a click on it is Confirm, a click on a row's arrow is Left or Right,
// a click on a list's choice chooses it, a click on a tab shows it.
#pragma once

namespace ui {

enum class RowKind {
    choice,  // one of a list of choices
    toggle,  // on or off
    number,  // a value in steps
    action,  // a button: does something
    heading, // a section's title: never has the focus
};

// What the model knows of a row, asked again before each move (a change can show, hide or disable rows)
struct RowShape {
    RowKind kind;
    bool enabled;      // false: shown dimmed, never has the focus
    int choice_count;  // choice: how many
    int current;       // choice: the chosen one (-1: none of them, a value set in settings.json); toggle: 0 or 1
};

class MenuRows {
public:
    virtual ~MenuRows() = default;
    virtual int tab_count() const = 0;
    virtual int row_count(int tab) const = 0;
    virtual RowShape shape(int tab, int row) const = 0;
    // a choice's choice, or a toggle's 0 / 1
    virtual void choose(int tab, int row, int choice) = 0;
    // a number: delta steps up (or down)
    virtual void step(int tab, int row, int delta) = 0;
    virtual void activate(int tab, int row) = 0;
};

enum class MenuInput { up, down, left, right, confirm, back, next_tab, previous_tab };

// What the menu is to do after an input
enum class MenuResult { stay, close };

class MenuModel {
public:
    explicit MenuModel(MenuRows &rows) : rows_(rows) {
    }

    // The menu opens on that tab, on its first row that can be used, with no list open
    void open_at(int tab);
    MenuResult press(MenuInput input);

    void point_at_row(int row);
    void click_row(int row);
    void click_step(int row, int delta);
    void click_choice(int choice);
    void click_tab(int tab);

    int tab() const {
        return tab_;
    }
    // the row with the focus, or -1 (the tab has no row that can be used)
    int row() const {
        return row_;
    }
    bool list_open() const {
        return list_open_;
    }
    // the open list's highlighted choice
    int list_choice() const {
        return list_choice_;
    }

private:
    bool usable(int row) const;
    int first_usable() const;
    int usable_from(int row, int direction) const;
    void settle(); // the focus and list kept to what the rows are now
    void step_row(int delta);
    void confirm_row();

    MenuRows &rows_;
    int tab_ = 0;
    int row_ = -1;
    bool list_open_ = false;
    int list_choice_ = 0;
};

} // namespace ui
