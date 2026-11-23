// The first-start setup (port/ui/setup_model.hpp, setup_pages.hpp), on stand-in rows: one for each row the tables
// name, holding every value the tables give it. A card sets its preset and the settings everyone gets, and goes on;
// Keep my settings sets only the depth; Skip only what everyone gets, and is done; Cancel (run again) puts every row
// back; the pages go on and back, the Welcome only by a card; the Controls page shows its device's rows; Finish is
// done; a table naming a row that is not there is a problem.
#include "setup_model.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <utility>
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

// ---- Stand-in rows: each a value of its own, read and set through plain functions ---------------------------------
constexpr int MAX_ROWS = 96;
constexpr int START = -7; // every row's value before the setup (none of the tables' values)
int values[MAX_ROWS];

template <int I> int get_value() {
    return values[I];
}
template <int I> void set_value(int value) {
    values[I] = value;
}
template <int... I> constexpr auto getters(std::integer_sequence<int, I...>) {
    return std::array<int (*)(), sizeof...(I)>{get_value<I>...};
}
template <int... I> constexpr auto setters(std::integer_sequence<int, I...>) {
    return std::array<void (*)(int), sizeof...(I)>{set_value<I>...};
}
const auto get_of = getters(std::make_integer_sequence<int, MAX_ROWS>());
const auto set_of = setters(std::make_integer_sequence<int, MAX_ROWS>());

struct FakeRow {
    std::string tab_id, label;
    std::vector<Choice> choices;
    SettingRow row;
};
std::vector<FakeRow> fakes;

FakeRow *fake(std::string_view tab_id, std::string_view label) {
    for (FakeRow &f : fakes) {
        if (f.tab_id == tab_id && f.label == label) {
            return &f;
        }
    }
    return nullptr;
}

void add(const char *tab_id, const char *label, int value) {
    FakeRow *f = fake(tab_id, label);
    if (f == nullptr) {
        fakes.push_back({tab_id, label, {}, {}});
        f = &fakes.back();
    }
    for (const Choice &c : f->choices) {
        if (c.value == value) {
            return;
        }
    }
    f->choices.push_back({"", value});
}

// One stand-in for every row the tables name, with each value they give it (and 0 and 1)
void make_fakes() {
    for (const SetupPage &page : setup_pages()) {
        for (const SetupRowRef &ref : page.rows) {
            if (ref.own == SetupOwn::none) {
                add(ref.tab_id, ref.label, 0);
                add(ref.tab_id, ref.label, 1);
            }
        }
    }
    for (const PresetValue &p : preset_values()) {
        for (int v : p.value) {
            add(p.tab_id, p.label, v);
        }
    }
    for (const GrantedValue &g : granted_values()) {
        add(g.tab_id, g.label, g.value);
    }
    CHECK(fakes.size() <= size_t(MAX_ROWS));
    for (size_t i = 0; i < fakes.size() && i < size_t(MAX_ROWS); i++) {
        FakeRow &f = fakes[i];
        f.row = SettingRow{f.label.c_str(), "", RowKind::choice, f.choices, {}, get_of[i], set_of[i]};
    }
}

const SettingRow *find(std::string_view tab_id, std::string_view label) {
    FakeRow *f = fake(tab_id, label);
    return f != nullptr ? &f->row : nullptr;
}

const SettingRow *find_nothing(std::string_view, std::string_view) {
    return nullptr;
}

int value_of(const char *tab_id, const char *label) {
    return fake(tab_id, label)->row.get();
}

void reset_values() {
    for (int &v : values) {
        v = START;
    }
}

bool all_at_start() {
    for (size_t i = 0; i < fakes.size(); i++) {
        if (values[i] != START) {
            return false;
        }
    }
    return true;
}

bool preset_set(Preset preset) {
    for (const PresetValue &p : preset_values()) {
        if (value_of(p.tab_id, p.label) != p.value[int(preset)]) {
            return false;
        }
    }
    return true;
}

bool granted_set(bool keeping) {
    for (const GrantedValue &g : granted_values()) {
        int wanted = !keeping || g.keep ? g.value : START;
        if (value_of(g.tab_id, g.label) != wanted) {
            return false;
        }
    }
    return true;
}

// The row of that own kind on the page, or -1
int own_row(const SetupFlow &flow, SetupOwn own) {
    for (int row = 0; row < flow.rows().row_count(flow.page()); row++) {
        if (flow.rows().own(flow.page(), row) == own) {
            return row;
        }
    }
    return -1;
}

bool shows(const SetupFlow &flow, const char *label) {
    for (int row = 0; row < flow.rows().row_count(flow.page()); row++) {
        if (std::strcmp(flow.rows().row(flow.page(), row).label, label) == 0) {
            return true;
        }
    }
    return false;
}

int last_page() {
    return int(setup_pages().size()) - 1;
}

// ---- The tests ----------------------------------------------------------------------------------------------------
void tables() {
    CHECK(setup_problems(find).empty());
    CHECK(!setup_problems(find_nothing).empty());
    CHECK(setup_pages().size() >= 3);
    CHECK(std::strcmp(setup_pages()[0].id, "welcome") == 0);
    // the last page has only Finish; every other page after the Welcome ends with Continue
    std::span<const SetupRowRef> last = setup_pages().back().rows;
    CHECK(last.size() == 1 && last[0].own == SetupOwn::finish);
    for (size_t page = 1; page + 1 < setup_pages().size(); page++) {
        CHECK(setup_pages()[page].rows.back().own == SetupOwn::next);
    }
}

void due() {
    CHECK(setup_due(0, false));
    CHECK(!setup_due(SETUP_VERSION, false));
    CHECK(setup_due(SETUP_VERSION, true));
}

void welcome_rows() {
    SetupFlow flow(find);
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    CHECK(flow.page() == 0);
    CHECK(own_row(flow, SetupOwn::card_keep) < 0 && own_row(flow, SetupOwn::cancel) < 0);
    CHECK(own_row(flow, SetupOwn::skip) >= 0);
    flow.open(SetupStart::again, true, SetupDevice::keyboard);
    CHECK(own_row(flow, SetupOwn::card_keep) >= 0 && own_row(flow, SetupOwn::cancel) >= 0);
    CHECK(own_row(flow, SetupOwn::skip) < 0);
}

void cards_and_pages() {
    reset_values();
    SetupFlow flow(find);
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    // the Welcome is left by a card only
    flow.press(MenuInput::next_tab);
    CHECK(flow.page() == 0 && flow.take_cue() == MenuCue::refused);
    flow.press(MenuInput::back);
    CHECK(flow.page() == 0);
    // the first card: Original
    CHECK(flow.rows().own(0, flow.model().row()) == SetupOwn::card_ps2);
    CHECK(flow.press(MenuInput::confirm) == SetupResult::stay);
    CHECK(flow.page() == 1 && preset_set(Preset::ps2) && granted_set(false));
    CHECK(flow.preset() == Preset::ps2);
    CHECK(flow.take_cue() == MenuCue::confirm);
    // on and back
    flow.press(MenuInput::next_tab);
    CHECK(flow.page() == 2 && flow.take_cue() == MenuCue::move);
    flow.press(MenuInput::previous_tab);
    CHECK(flow.page() == 1 && flow.take_cue() == MenuCue::back);
    flow.press(MenuInput::back);
    CHECK(flow.page() == 0);
    // another card puts its own values in
    flow.press(MenuInput::down);
    flow.press(MenuInput::down);
    CHECK(flow.rows().own(0, flow.model().row()) == SetupOwn::card_modern);
    flow.press(MenuInput::confirm);
    CHECK(flow.page() == 1 && preset_set(Preset::modern) && flow.preset() == Preset::modern);
    // a page reached can be clicked in the sidebar, one not reached cannot
    flow.click_page(3);
    CHECK(flow.page() == 1);
    // Continue at the end of a page
    int next = own_row(flow, SetupOwn::next);
    CHECK(next >= 0);
    CHECK(flow.click_row(next) == SetupResult::stay && flow.page() == 2);
    flow.click_page(1);
    CHECK(flow.page() == 1);
    // to the last page, where Next page does nothing and Finish is done
    for (int i = 0; i < 20; i++) {
        flow.press(MenuInput::next_tab);
    }
    CHECK(flow.page() == last_page());
    CHECK(flow.rows().own(flow.page(), flow.model().row()) == SetupOwn::finish);
    CHECK(flow.press(MenuInput::confirm) == SetupResult::done);
}

void controls_device() {
    reset_values();
    SetupFlow flow(find);
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    flow.press(MenuInput::confirm); // (a card: to the Controls page)
    CHECK(std::strcmp(setup_pages()[size_t(flow.page())].id, "controls") == 0);
    CHECK(shows(flow, "Mouse Sensitivity") && !shows(flow, "Twin-Stick Controls"));
    CHECK(flow.rows().own(flow.page(), flow.model().row()) == SetupOwn::device);
    flow.press(MenuInput::right);
    CHECK(flow.device() == SetupDevice::pad);
    CHECK(!shows(flow, "Mouse Sensitivity") && shows(flow, "Twin-Stick Controls"));
    CHECK(flow.rows().value_text(flow.page(), flow.model().row()) == "Controller");
    flow.open(SetupStart::first, false, SetupDevice::pad);
    CHECK(flow.device() == SetupDevice::pad);
}

void skip_keep_cancel() {
    // Skip: only what everyone gets
    reset_values();
    SetupFlow flow(find);
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    int skip = own_row(flow, SetupOwn::skip);
    CHECK(skip >= 0 && flow.click_row(skip) == SetupResult::done);
    CHECK(granted_set(false));
    for (const PresetValue &p : preset_values()) {
        bool granted = false;
        for (const GrantedValue &g : granted_values()) {
            granted = granted || (std::strcmp(g.tab_id, p.tab_id) == 0 && std::strcmp(g.label, p.label) == 0);
        }
        CHECK(granted || value_of(p.tab_id, p.label) == START);
    }

    // Keep my settings: the depth only
    reset_values();
    flow.open(SetupStart::again, true, SetupDevice::keyboard);
    int keep = own_row(flow, SetupOwn::card_keep);
    CHECK(keep >= 0 && flow.click_row(keep) == SetupResult::stay && flow.page() == 1);
    CHECK(granted_set(true));

    // Cancel, run again: a card, a change, then back to the Welcome and Esc: every row as it was
    reset_values();
    flow.open(SetupStart::again, true, SetupDevice::keyboard);
    flow.press(MenuInput::confirm);
    CHECK(!all_at_start());
    flow.press(MenuInput::back);
    CHECK(flow.page() == 0);
    CHECK(flow.press(MenuInput::back) == SetupResult::done);
    CHECK(all_at_start());

    // the first start has no Cancel: Esc on the Welcome is refused
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    CHECK(flow.press(MenuInput::back) == SetupResult::stay && flow.take_cue() == MenuCue::refused);
}

void list_closes_first() {
    reset_values();
    SetupFlow flow(find);
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    flow.press(MenuInput::confirm); // (Controls: its first row, the device, a choice)
    flow.press(MenuInput::confirm);
    CHECK(flow.model().list_open());
    flow.press(MenuInput::back);
    CHECK(!flow.model().list_open() && flow.page() == 1);
}

} // namespace

// ---- Emulator Saves, on a stand-in list ------------------------------------------------------------------------
class FakeImports final : public SaveImports {
public:
    int saves = 0;
    int free_slots = 1;
    int looks = 0, choosers = 0;
    std::vector<int> imported;
    std::string chosen;

    void look() override {
        looks++;
    }
    void update() override {
    }
    int count() const override {
        return saves;
    }
    const SettingRow &row(int) const override {
        return row_;
    }
    std::string state(int save) const override {
        return is_imported(save) ? "Imported" : importable(save) ? "Import" : "No free slot";
    }
    bool importable(int save) const override {
        return !is_imported(save) && int(imported.size()) < free_slots;
    }
    bool import(int save) override {
        if (!importable(save)) {
            return false;
        }
        imported.push_back(save);
        return true;
    }
    void choose_file() override {
        choosers++;
    }
    std::string chosen_text() const override {
        return chosen;
    }

private:
    bool is_imported(int save) const {
        for (int i : imported) {
            if (i == save) {
                return true;
            }
        }
        return false;
    }
    SettingRow row_{"Card save", "", RowKind::action};
};

int page_of(const char *id) {
    for (size_t page = 0; page < setup_pages().size(); page++) {
        if (std::strcmp(setup_pages()[page].id, id) == 0) {
            return int(page);
        }
    }
    return -1;
}

void emulator_saves() {
    reset_values();
    FakeImports imports;
    SetupFlow flow(find, &imports);
    flow.open(SetupStart::first, false, SetupDevice::keyboard);
    CHECK(imports.looks == 1);
    int page = page_of("saves");
    CHECK(page > 0 && page < last_page());
    flow.show_page(page);
    // none found: the line says so; the chooser's row and Next
    CHECK(own_row(flow, SetupOwn::emulator_save) < 0 && own_row(flow, SetupOwn::emulator_none) >= 0);
    int browse = own_row(flow, SetupOwn::emulator_browse);
    CHECK(browse >= 0 && own_row(flow, SetupOwn::next) > browse);
    CHECK(flow.click_row(browse) == SetupResult::stay && imports.choosers == 1 && flow.page() == page);
    imports.chosen = "Not a PS2 memory card";
    CHECK(flow.rows().value_text(page, browse) == "Not a PS2 memory card");

    // two found, room for one: a row each, each its state; the second refused once the first is in
    imports.saves = 2;
    flow.show_page(page);
    CHECK(own_row(flow, SetupOwn::emulator_none) < 0 && flow.rows().row_count(page) == 4);
    CHECK(flow.rows().own(page, 0) == SetupOwn::emulator_save && flow.rows().own(page, 1) == SetupOwn::emulator_save);
    CHECK(flow.rows().value_text(page, 1) == "Import");
    flow.take_cue();
    CHECK(flow.click_row(1) == SetupResult::stay && imports.imported.size() == 1 && imports.imported[0] == 1);
    CHECK(flow.take_cue() == MenuCue::confirm && flow.rows().value_text(page, 1) == "Imported");
    CHECK(!flow.rows().shape(page, 0).enabled && !flow.rows().shape(page, 1).enabled);
    CHECK(flow.model().row() == own_row(flow, SetupOwn::emulator_browse)); // (the focus off the dim rows)
    flow.click_row(0);
    CHECK(imports.imported.size() == 1);

    // at most SETUP_EMULATOR_SAVES rows
    imports.saves = SETUP_EMULATOR_SAVES + 3;
    CHECK(flow.rows().row_count(page) == SETUP_EMULATOR_SAVES + 2);

    // without a list: the line, no chooser
    SetupFlow bare(find);
    bare.open(SetupStart::first, false, SetupDevice::keyboard);
    bare.show_page(page);
    CHECK(own_row(bare, SetupOwn::emulator_none) >= 0 && own_row(bare, SetupOwn::emulator_browse) < 0);
}

int main() {
    make_fakes();
    tables();
    due();
    welcome_rows();
    cards_and_pages();
    controls_device();
    skip_keep_cancel();
    list_closes_first();
    emulator_saves();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ui_setup: all passed\n");
    return 0;
}
