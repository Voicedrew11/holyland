// The first-start setup's moving about (the pages: port/ui/setup_pages.hpp). No RmlUi; the rows are the player menu's
// (found by a RowFinder: find_setting_row in the game, stand-ins in tests/ui_setup_test.cpp), set as the player steps,
// so the game behind shows each change at once.
//
//   The Welcome     a card sets every preset value and goes on; Keep my settings goes on with nothing changed; Skip
//                   gives only the settings everyone gets and finishes; Cancel (run again from the menu) puts back
//                   everything as it was when the setup opened
//   Other pages     the rows move as the player menu's (MenuModel); Continue, Next page (E, R1) go on; Back (Esc,
//                   Circle) and Previous page (Q, L1) go back a page (a list open: closes it first)
//   Emulator Saves  the saves found on PCSX2's and RetroArch's cards (a SaveImports), each imported by its row into
//                   a free save; My save is in a different location opens the system's file chooser
//   The last page   Finish
// The settings every player gets go in with a card (or Skip); the depth goes in with Keep my settings too.
#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "emulator_saves.hpp"
#include "menu_model.hpp"
#include "setup_pages.hpp"

namespace ui {

// The setup's version: a player whose settings.json says less sees it at the next start. Raised when a page that
// everyone should see is added.
inline constexpr int SETUP_VERSION = 1;

// The setup shows at this start: the player has not gone through this version, or it is asked for (KF4_SETUP=1)
bool setup_due(int seen_version, bool forced);

enum class SetupStart {
    first, // at the start of the game: no Cancel
    again, // from the player menu: Cancel puts back what was there
};

enum class SetupResult { stay, done };

class SetupFlow;

// The setup's pages as the model's and the view's rows: page n is tab n, each row the player menu's or the setup's own
class SetupRows final : public RowSource {
public:
    explicit SetupRows(SetupFlow &flow) : flow_(flow) {
    }
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
    std::string row_classes(int tab, int row) const override; // the Welcome's cards: "card", the lit one "lit"

    // the setup's own row at that place, or SetupOwn::none for a player menu row
    SetupOwn own(int tab, int row) const;

private:
    const SetupRowRef &ref(int tab, int row) const;
    SetupFlow &flow_;
};

class SetupFlow {
public:
    // imports: the Emulator Saves page's saves (nullptr: none found, and no chooser). Kept, not owned.
    explicit SetupFlow(RowFinder find, SaveImports *imports = nullptr);
    SetupFlow(const SetupFlow &) = delete;
    SetupFlow &operator=(const SetupFlow &) = delete;

    // Opens on the Welcome. had_settings: offer Keep my settings. device: what the Controls page starts on.
    void open(SetupStart start, bool had_settings, SetupDevice device);
    SetupResult press(MenuInput input);
    // the pointer, as the player menu's (MenuModel)
    void point_at_row(int row);
    SetupResult click_row(int row);
    void click_step(int row, int delta);
    void click_choice(int choice);
    // a page in the sidebar: only one already reached
    void click_page(int page);
    // the sound for the last input (as MenuModel::take_cue), the page's moves included
    MenuCue take_cue();
    // (the test pictures: KF4_OVERLAY_SHOT's setup-<page>) straight to that page, as if every page before it was seen
    void show_page(int page);

    int page() const {
        return model_.tab();
    }
    int pages_reached() const {
        return reached_;
    }
    const MenuModel &model() const {
        return model_;
    }
    MenuModel &model() {
        return model_;
    }
    const SetupRows &rows() const {
        return rows_;
    }
    SetupDevice device() const {
        return device_;
    }
    // the card chosen, or else the preset every value now matches (the Welcome lights it)
    std::optional<Preset> preset() const;
    bool again() const {
        return start_ == SetupStart::again;
    }
    bool had_settings() const {
        return had_settings_;
    }
    // a row is shown on its page (its device, its own shown())
    bool shown(const SetupRowRef &ref) const;
    // a page's n-th shown row (the caller keeps n below the count)
    const SetupRowRef &shown_ref(int page, int n) const;
    int shown_count(int page) const;
    const SettingRow *find(const SetupRowRef &ref) const;
    SaveImports *imports() const {
        return imports_;
    }

private:
    friend class SetupRows;
    void set_device(SetupDevice device);
    SetupResult take(SetupOwn own); // a setup's own row done
    void go_to(int page);
    void apply(Preset preset);
    void grant(bool keeping);
    void remember(const char *tab_id, const char *label);
    void put_back();

    RowFinder find_;
    SaveImports *imports_;
    SetupRows rows_{*this};
    MenuModel model_{rows_};
    SetupStart start_ = SetupStart::first;
    bool had_settings_ = false;
    SetupDevice device_ = SetupDevice::keyboard;
    std::optional<Preset> chosen_;
    int reached_ = 0;                                    // the furthest page gone to
    SetupOwn pending_ = SetupOwn::none;                  // an own row activated, for press / click_row to take
    int pending_item_ = 0;                               // and its item (emulator_save)
    MenuCue cue_ = MenuCue::none;                        // a page's move (the model's are its own)
    std::vector<std::pair<const SettingRow *, int>> was_; // every row the setup can set, as it was at open
};

} // namespace ui
