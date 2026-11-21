// The player menu's moving about (port/ui/menu_model.hpp), on stand-in rows: the focus skips headings and disabled
// rows and stops at the ends; Left and Right step choices (not past the ends), turn toggles and step numbers;
// Confirm opens a choice's list, which Up, Down, Confirm and Back work; tabs go round; a row that is hidden or
// disabled under the focus gives it to its neighbour; the pointer's clicks do what the keys do.
#include "menu_model.hpp"

#include <cstdio>
#include <vector>

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

struct FakeRow {
    RowKind kind;
    bool enabled = true;
    int choices = 0;
    int value = 0; // choice: the chosen; toggle: 0 / 1; number: the value
    int activated = 0;
    int listened = -1; // a binding: the slot last listened for
    int cleared = -1;  // and last cleared
};

class FakeRows final : public MenuRows {
public:
    std::vector<std::vector<FakeRow>> tabs;

    int tab_count() const override {
        return int(tabs.size());
    }
    int row_count(int tab) const override {
        return int(tabs[tab].size());
    }
    RowShape shape(int tab, int row) const override {
        const FakeRow &r = tabs[tab][row];
        return {r.kind, r.enabled, r.choices, r.value};
    }
    void choose(int tab, int row, int choice) override {
        tabs[tab][row].value = choice;
    }
    void step(int tab, int row, int delta) override {
        tabs[tab][row].value += delta;
    }
    void activate(int tab, int row) override {
        tabs[tab][row].activated++;
    }
    void listen(int tab, int row, int slot) override {
        tabs[tab][row].listened = slot;
    }
    void clear(int tab, int row, int slot) override {
        tabs[tab][row].cleared = slot;
    }
};

// Tab 0: heading, choice (3), toggle, disabled toggle, number, action. Tab 1: heading only. Tab 2: one toggle.
FakeRows make_rows() {
    FakeRows rows;
    rows.tabs = {
        {{RowKind::heading}, {RowKind::choice, true, 3, 1}, {RowKind::toggle}, {RowKind::toggle, false},
         {RowKind::number, true, 0, 10}, {RowKind::action}},
        {{RowKind::heading}},
        {{RowKind::toggle}},
    };
    return rows;
}

void focus_skips_and_stops() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK(menu.row() == 1); // past the heading
    menu.press(MenuInput::up);
    CHECK(menu.row() == 1); // not past the top
    menu.press(MenuInput::down);
    CHECK(menu.row() == 2);
    menu.press(MenuInput::down);
    CHECK(menu.row() == 4); // past the disabled toggle
    menu.press(MenuInput::down);
    menu.press(MenuInput::down);
    CHECK(menu.row() == 5); // not past the bottom
}

void left_right() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][1].value == 2);
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][1].value == 2); // not past the last choice
    menu.press(MenuInput::left);
    menu.press(MenuInput::left);
    menu.press(MenuInput::left);
    CHECK(rows.tabs[0][1].value == 0);
    // a value that is none of the choices: Right gives the first, Left the last
    rows.tabs[0][1].value = -1;
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][1].value == 0);
    rows.tabs[0][1].value = -1;
    menu.press(MenuInput::left);
    CHECK(rows.tabs[0][1].value == 2);

    menu.press(MenuInput::down); // the toggle
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][2].value == 1);
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][2].value == 1);
    menu.press(MenuInput::left);
    CHECK(rows.tabs[0][2].value == 0);

    menu.press(MenuInput::down); // the number
    menu.press(MenuInput::right);
    menu.press(MenuInput::right);
    menu.press(MenuInput::left);
    CHECK(rows.tabs[0][4].value == 11);
}

void confirm_and_lists() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::confirm);
    CHECK(menu.list_open() && menu.list_choice() == 1); // on the chosen one
    menu.press(MenuInput::down);
    menu.press(MenuInput::down);
    CHECK(menu.list_choice() == 2); // not past the end
    CHECK(rows.tabs[0][1].value == 1); // nothing chosen yet
    CHECK(menu.press(MenuInput::back) == MenuResult::stay);
    CHECK(!menu.list_open() && rows.tabs[0][1].value == 1); // Back: nothing chosen
    menu.press(MenuInput::confirm);
    menu.press(MenuInput::up);
    menu.press(MenuInput::confirm);
    CHECK(!menu.list_open() && rows.tabs[0][1].value == 0);
    // in a list, Left and Right do nothing and Up and Down move only the list
    menu.press(MenuInput::confirm);
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][1].value == 0 && menu.row() == 1);
    menu.press(MenuInput::back);

    menu.press(MenuInput::down); // the toggle: Confirm turns it over
    menu.press(MenuInput::confirm);
    CHECK(rows.tabs[0][2].value == 1 && !menu.list_open());
    menu.press(MenuInput::confirm);
    CHECK(rows.tabs[0][2].value == 0);

    menu.press(MenuInput::down);
    menu.press(MenuInput::down); // the action
    menu.press(MenuInput::confirm);
    CHECK(rows.tabs[0][5].activated == 1);
    CHECK(menu.press(MenuInput::back) == MenuResult::close);
}

void tabs() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::confirm); // a list open: a tab change closes it
    menu.press(MenuInput::next_tab);
    CHECK(menu.tab() == 1 && !menu.list_open());
    CHECK(menu.row() == -1); // nothing to use there
    menu.press(MenuInput::down);
    menu.press(MenuInput::confirm); // nothing happens
    CHECK(menu.row() == -1);
    menu.press(MenuInput::next_tab);
    CHECK(menu.tab() == 2 && menu.row() == 0);
    menu.press(MenuInput::next_tab);
    CHECK(menu.tab() == 0 && menu.row() == 1); // round
    menu.press(MenuInput::previous_tab);
    CHECK(menu.tab() == 2);
    menu.open_at(7); // no such tab: the first
    CHECK(menu.tab() == 0);
}

void rows_change_under_the_focus() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.press(MenuInput::down); // the toggle, row 2
    rows.tabs[0][2].enabled = false;
    menu.press(MenuInput::left); // settles first: the focus goes on to the number
    CHECK(menu.row() == 4);
    CHECK(rows.tabs[0][4].value == 9);
    // the rows below go: the focus goes back up
    rows.tabs[0].resize(3);
    menu.press(MenuInput::down);
    CHECK(menu.row() == 1);
    // the list's row stops being a choice: the list closes
    menu.press(MenuInput::confirm);
    CHECK(menu.list_open());
    rows.tabs[0][1].kind = RowKind::action;
    menu.press(MenuInput::down);
    CHECK(!menu.list_open());
}

void pointer() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    menu.point_at_row(4);
    CHECK(menu.row() == 4);
    menu.point_at_row(3); // disabled: the focus stays
    menu.point_at_row(0); // a heading
    CHECK(menu.row() == 4);
    menu.click_row(2);
    CHECK(menu.row() == 2 && rows.tabs[0][2].value == 1);
    menu.click_step(1, 1);
    CHECK(menu.row() == 1 && rows.tabs[0][1].value == 2);
    menu.click_row(1);
    CHECK(menu.list_open());
    menu.point_at_row(5); // a list open: the pointer leaves the focus
    CHECK(menu.row() == 1);
    menu.click_choice(0);
    CHECK(!menu.list_open() && rows.tabs[0][1].value == 0);
    menu.click_row(1);
    menu.click_choice(9); // no such choice
    CHECK(menu.list_open() && rows.tabs[0][1].value == 0);
    menu.click_row(4); // a click outside the list closes it, and does nothing else
    CHECK(!menu.list_open() && menu.row() == 1);
    menu.click_tab(2);
    CHECK(menu.tab() == 2 && menu.row() == 0);
}

} // namespace

// Bindings: Left / Right choose the slot (not past the ends) and keep it from row to row; Confirm listens there,
// Clear empties it; a click on a slot listens there; Clear does nothing on other rows
void bindings() {
    FakeRows rows;
    rows.tabs = {{{RowKind::heading}, {RowKind::binding, true, 3}, {RowKind::binding, true, 3}, {RowKind::toggle}}};
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK(menu.row() == 1 && menu.slot() == 0);
    menu.press(MenuInput::left);
    CHECK(menu.slot() == 0);
    menu.press(MenuInput::right);
    menu.press(MenuInput::right);
    menu.press(MenuInput::right);
    CHECK(menu.slot() == 2);
    menu.press(MenuInput::confirm);
    CHECK(rows.tabs[0][1].listened == 2 && !menu.list_open());
    menu.press(MenuInput::down);
    CHECK(menu.row() == 2 && menu.slot() == 2);
    menu.press(MenuInput::left);
    menu.press(MenuInput::clear);
    CHECK(rows.tabs[0][2].cleared == 1 && rows.tabs[0][2].listened == -1);
    menu.press(MenuInput::down);
    menu.press(MenuInput::clear);
    CHECK(rows.tabs[0][3].value == 0);
    menu.click_slot(1, 0);
    CHECK(menu.row() == 1 && menu.slot() == 0 && rows.tabs[0][1].listened == 0);
    menu.click_slot(3, 1); // not a binding: nothing
    CHECK(menu.row() == 1 && rows.tabs[0][3].value == 0);
    menu.click_slot(2, 3); // no such slot
    CHECK(menu.row() == 1);
}

// The sound each input makes: a move only when something moved or changed, refused for Confirm with nothing to do
void cues() {
    FakeRows rows = make_rows();
    MenuModel menu(rows);
    menu.open_at(0);
    CHECK(menu.take_cue() == MenuCue::none);
    menu.press(MenuInput::up); // already the first usable row
    CHECK(menu.take_cue() == MenuCue::none);
    menu.press(MenuInput::down);
    CHECK(menu.row() == 2 && menu.take_cue() == MenuCue::move);
    CHECK(menu.take_cue() == MenuCue::none); // taken once
    menu.press(MenuInput::left); // the toggle is off already
    CHECK(menu.take_cue() == MenuCue::none);
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][2].value == 1 && menu.take_cue() == MenuCue::move);
    menu.press(MenuInput::confirm);
    CHECK(rows.tabs[0][2].value == 0 && menu.take_cue() == MenuCue::confirm);
    menu.press(MenuInput::down); // past the disabled row to the number
    CHECK(menu.row() == 4 && menu.take_cue() == MenuCue::move);
    menu.press(MenuInput::confirm); // a number: nothing to do
    CHECK(menu.take_cue() == MenuCue::refused);
    menu.press(MenuInput::clear); // not a binding
    CHECK(menu.take_cue() == MenuCue::none);
    menu.press(MenuInput::down);
    menu.press(MenuInput::down); // the last row: stays
    CHECK(menu.row() == 5 && menu.take_cue() == MenuCue::move);
    menu.press(MenuInput::confirm);
    CHECK(rows.tabs[0][5].activated == 1 && menu.take_cue() == MenuCue::confirm);

    // a choice's list
    menu.open_at(0);
    menu.press(MenuInput::confirm);
    CHECK(menu.list_open() && menu.take_cue() == MenuCue::confirm);
    menu.press(MenuInput::down);
    CHECK(menu.take_cue() == MenuCue::move);
    menu.press(MenuInput::down); // the last choice
    CHECK(menu.take_cue() == MenuCue::none);
    menu.press(MenuInput::back);
    CHECK(!menu.list_open() && menu.take_cue() == MenuCue::back);
    menu.press(MenuInput::right);
    CHECK(rows.tabs[0][1].value == 2 && menu.take_cue() == MenuCue::move);
    menu.press(MenuInput::right); // the last choice
    CHECK(menu.take_cue() == MenuCue::none);
    menu.press(MenuInput::confirm);
    menu.press(MenuInput::confirm);
    CHECK(!menu.list_open() && menu.take_cue() == MenuCue::confirm);
    CHECK(menu.press(MenuInput::back) == MenuResult::close && menu.take_cue() == MenuCue::back);

    // tabs and the pointer
    menu.press(MenuInput::next_tab);
    CHECK(menu.tab() == 1 && menu.take_cue() == MenuCue::move);
    menu.click_tab(1); // the tab shown
    CHECK(menu.take_cue() == MenuCue::none);
    menu.click_tab(0);
    CHECK(menu.take_cue() == MenuCue::move);
    menu.point_at_row(1); // the row with the focus
    CHECK(menu.take_cue() == MenuCue::none);
    menu.point_at_row(4);
    CHECK(menu.row() == 4 && menu.take_cue() == MenuCue::move);
    menu.point_at_row(0); // a heading
    CHECK(menu.take_cue() == MenuCue::none);
    menu.click_row(3); // dimmed
    CHECK(menu.row() == 4 && menu.take_cue() == MenuCue::refused);
    menu.click_step(4, 1);
    CHECK(rows.tabs[0][4].value == 11 && menu.take_cue() == MenuCue::move);
    menu.click_row(1);
    CHECK(menu.list_open() && menu.take_cue() == MenuCue::confirm);
    menu.click_choice(0);
    CHECK(rows.tabs[0][1].value == 0 && menu.take_cue() == MenuCue::confirm);
    menu.click_row(1);
    menu.click_row(-1); // outside: closes the list
    CHECK(!menu.list_open() && menu.take_cue() == MenuCue::back);
}

int main() {
    focus_skips_and_stops();
    left_right();
    confirm_and_lists();
    tabs();
    rows_change_under_the_focus();
    pointer();
    bindings();
    cues();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("ui_menu: all passed\n");
    return 0;
}
