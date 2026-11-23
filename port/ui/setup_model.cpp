// The first-start setup's moving about (port/ui/setup_model.hpp)
#include "setup_model.hpp"

#include <algorithm>
#include <span>

namespace ui {

bool setup_due(int seen_version, bool forced) {
    return forced || seen_version < SETUP_VERSION;
}

namespace {

bool own_is_card(SetupOwn own) {
    return own == SetupOwn::card_ps2 || own == SetupOwn::card_balanced || own == SetupOwn::card_modern;
}

Preset card_preset(SetupOwn own) {
    switch (own) {
    case SetupOwn::card_balanced:
        return Preset::balanced;
    case SetupOwn::card_modern:
        return Preset::modern;
    default:
        return Preset::ps2;
    }
}

// A row the setup can set and put back: it reads and writes a whole value
bool settable(const SettingRow *row) {
    return row != nullptr && row->get != nullptr && row->set != nullptr &&
           (row->kind == RowKind::choice || row->kind == RowKind::toggle || row->kind == RowKind::number);
}

} // namespace

// ---- The rows -----------------------------------------------------------------------------------------------------
int SetupRows::tab_count() const {
    return int(setup_pages().size());
}

int SetupRows::row_count(int tab) const {
    return flow_.shown_count(tab);
}

const SetupRowRef &SetupRows::ref(int tab, int row) const {
    return flow_.shown_ref(tab, row);
}

SetupOwn SetupRows::own(int tab, int row) const {
    return ref(tab, row).own;
}

const SettingRow &SetupRows::row(int tab, int row) const {
    const SetupRowRef &r = ref(tab, row);
    if (r.own == SetupOwn::emulator_save) {
        return flow_.imports_->row(r.item); // (shown only for a save found)
    }
    if (r.own != SetupOwn::none) {
        return setup_own_row(r.own);
    }
    return *flow_.find(r); // (a shown row was found)
}

RowShape SetupRows::shape(int tab, int index) const {
    SetupOwn o = own(tab, index);
    if (o == SetupOwn::device) {
        return {RowKind::choice, true, 2, flow_.device() == SetupDevice::pad ? 1 : 0};
    }
    if (o == SetupOwn::emulator_save) {
        return {RowKind::action, flow_.imports_->importable(ref(tab, index).item), 0, 0};
    }
    if (o == SetupOwn::emulator_none) {
        return {RowKind::heading, true, 0, 0};
    }
    if (o != SetupOwn::none) {
        return {RowKind::action, true, 0, 0};
    }
    return row_shape(row(tab, index));
}

void SetupRows::choose(int tab, int index, int choice) {
    if (own(tab, index) == SetupOwn::device) {
        flow_.set_device(choice == 1 ? SetupDevice::pad : SetupDevice::keyboard);
        return;
    }
    if (own(tab, index) == SetupOwn::none) {
        row_choose(row(tab, index), choice);
    }
}

void SetupRows::step(int tab, int index, int delta) {
    if (own(tab, index) == SetupOwn::none) {
        row_step(row(tab, index), delta);
    }
}

void SetupRows::activate(int tab, int index) {
    SetupOwn o = own(tab, index);
    if (o != SetupOwn::none) {
        flow_.pending_ = o; // (done by the flow once the model has the input: SetupFlow::take)
        flow_.pending_item_ = ref(tab, index).item;
        return;
    }
    row_activate(row(tab, index));
}

void SetupRows::listen(int, int, int) {
}

void SetupRows::clear(int, int, int) {
}

bool SetupRows::choice_enabled(int tab, int index, int choice) const {
    if (own(tab, index) != SetupOwn::none) {
        return true;
    }
    return row_choice_enabled(row(tab, index), choice);
}

std::string SetupRows::value_text(int tab, int index) const {
    SetupOwn o = own(tab, index);
    if (o == SetupOwn::device) {
        return row(tab, index).choices[flow_.device() == SetupDevice::pad ? 1 : 0].label;
    }
    if (o == SetupOwn::emulator_save) {
        return flow_.imports_->state(ref(tab, index).item);
    }
    if (o == SetupOwn::emulator_browse) {
        return flow_.imports_->chosen_text();
    }
    if (o != SetupOwn::none) {
        return "";
    }
    return row_value_text(row(tab, index));
}

std::string SetupRows::slot_text(int, int, int) const {
    return "-";
}

bool SetupRows::listening(int, int, int) const {
    return false;
}

bool SetupRows::changed_from_default(int, int) const {
    return false;
}

float SetupRows::number_fraction(int tab, int index) const {
    return own(tab, index) == SetupOwn::none ? row_number_fraction(row(tab, index)) : 0.0f;
}

std::string SetupRows::row_classes(int tab, int index) const {
    SetupOwn o = own(tab, index);
    if (own_is_card(o)) {
        std::optional<Preset> lit = flow_.preset();
        return lit && *lit == card_preset(o) ? "card lit" : "card";
    }
    if (o == SetupOwn::card_keep) {
        return "card";
    }
    if (o == SetupOwn::next || o == SetupOwn::finish) {
        return "next";
    }
    return {};
}

// ---- The flow -----------------------------------------------------------------------------------------------------
SetupFlow::SetupFlow(RowFinder find, SaveImports *imports) : find_(find), imports_(imports) {
}

const SettingRow *SetupFlow::find(const SetupRowRef &ref) const {
    return ref.own == SetupOwn::none && ref.tab_id != nullptr ? find_(ref.tab_id, ref.label) : nullptr;
}

bool SetupFlow::shown(const SetupRowRef &ref) const {
    switch (ref.own) {
    case SetupOwn::card_keep:
        return had_settings_;
    case SetupOwn::skip:
        return start_ == SetupStart::first;
    case SetupOwn::cancel:
        return start_ == SetupStart::again;
    case SetupOwn::emulator_save:
        return imports_ != nullptr && ref.item < imports_->count();
    case SetupOwn::emulator_none:
        return imports_ == nullptr || imports_->count() == 0;
    case SetupOwn::emulator_browse:
        return imports_ != nullptr;
    case SetupOwn::none:
        break;
    default:
        return true;
    }
    if (ref.device != SetupDevice::any && ref.device != device_) {
        return false;
    }
    const SettingRow *row = find(ref);
    return row != nullptr && (row->shown == nullptr || row->shown());
}

int SetupFlow::shown_count(int page) const {
    std::span<const SetupPage> pages = setup_pages();
    if (page < 0 || size_t(page) >= pages.size()) {
        return 0;
    }
    int count = 0;
    for (const SetupRowRef &ref : pages[size_t(page)].rows) {
        count += shown(ref);
    }
    return count;
}

const SetupRowRef &SetupFlow::shown_ref(int page, int n) const {
    std::span<const SetupRowRef> refs = setup_pages()[size_t(page)].rows;
    for (const SetupRowRef &ref : refs) {
        if (shown(ref) && n-- == 0) {
            return ref;
        }
    }
    return refs[0];
}

void SetupFlow::remember(const char *tab_id, const char *label) {
    const SettingRow *row = find_(tab_id, label);
    if (!settable(row)) {
        return;
    }
    auto same = [row](const std::pair<const SettingRow *, int> &kept) { return kept.first == row; };
    if (std::find_if(was_.begin(), was_.end(), same) == was_.end()) {
        was_.emplace_back(row, row->get());
    }
}

void SetupFlow::open(SetupStart start, bool had_settings, SetupDevice device) {
    start_ = start;
    had_settings_ = had_settings;
    device_ = device == SetupDevice::pad ? SetupDevice::pad : SetupDevice::keyboard;
    chosen_.reset();
    reached_ = 0;
    pending_ = SetupOwn::none;
    cue_ = MenuCue::none;
    was_.clear();
    if (imports_ != nullptr) {
        imports_->look();
    }
    for (const SetupPage &page : setup_pages()) {
        for (const SetupRowRef &ref : page.rows) {
            if (ref.own == SetupOwn::none) {
                remember(ref.tab_id, ref.label);
            }
        }
    }
    for (const PresetValue &value : preset_values()) {
        remember(value.tab_id, value.label);
    }
    for (const GrantedValue &value : granted_values()) {
        remember(value.tab_id, value.label);
    }
    model_.open_at(0);
}

void SetupFlow::put_back() {
    for (const auto &[row, value] : was_) {
        if (row->get() != value) {
            row->set(value);
        }
    }
}

void SetupFlow::apply(Preset preset) {
    for (const PresetValue &value : preset_values()) {
        const SettingRow *row = find_(value.tab_id, value.label);
        int wanted = value.value[int(preset)];
        if (settable(row) && row->get() != wanted) {
            row->set(wanted);
        }
    }
}

void SetupFlow::grant(bool keeping) {
    for (const GrantedValue &value : granted_values()) {
        const SettingRow *row = find_(value.tab_id, value.label);
        if ((!keeping || value.keep) && settable(row) && row->get() != value.value) {
            row->set(value.value);
        }
    }
}

void SetupFlow::set_device(SetupDevice device) {
    device_ = device;
}

std::optional<Preset> SetupFlow::preset() const {
    if (chosen_) {
        return chosen_;
    }
    for (int p = 0; p < PRESET_COUNT; p++) {
        bool all = true;
        for (const PresetValue &value : preset_values()) {
            const SettingRow *row = find_(value.tab_id, value.label);
            all = all && settable(row) && row->get() == value.value[p];
        }
        if (all) {
            return Preset(p);
        }
    }
    return std::nullopt;
}

void SetupFlow::go_to(int page) {
    int last = int(setup_pages().size()) - 1;
    page = std::clamp(page, 0, last);
    if (page == model_.tab()) {
        return;
    }
    cue_ = page < model_.tab() ? MenuCue::back : MenuCue::move;
    model_.open_at(page);
    reached_ = std::max(reached_, page);
}

SetupResult SetupFlow::take(SetupOwn own) {
    if (own_is_card(own)) {
        chosen_ = card_preset(own);
        apply(*chosen_);
        grant(false);
        go_to(1);
        cue_ = MenuCue::confirm;
        return SetupResult::stay;
    }
    switch (own) {
    case SetupOwn::card_keep:
        chosen_.reset();
        grant(true);
        go_to(1);
        cue_ = MenuCue::confirm;
        break;
    case SetupOwn::skip:
        grant(false);
        return SetupResult::done;
    case SetupOwn::cancel:
        put_back();
        return SetupResult::done;
    case SetupOwn::next:
        go_to(model_.tab() + 1);
        cue_ = MenuCue::confirm;
        break;
    case SetupOwn::finish:
        return SetupResult::done;
    case SetupOwn::emulator_save:
        cue_ = imports_ != nullptr && imports_->import(pending_item_) ? MenuCue::confirm : MenuCue::refused;
        model_.rows_changed(); // (the imported save's row is dim now)
        break;
    case SetupOwn::emulator_browse:
        if (imports_ != nullptr) {
            imports_->choose_file();
        }
        cue_ = MenuCue::confirm;
        break;
    default:
        break;
    }
    return SetupResult::stay;
}

SetupResult SetupFlow::press(MenuInput input) {
    if (model_.list_open()) {
        model_.press(input); // (Back, Next or Previous page close the list first)
        return SetupResult::stay;
    }
    int page = model_.tab();
    int last = int(setup_pages().size()) - 1;
    switch (input) {
    case MenuInput::next_tab:
        // (the Welcome is left by a card only)
        if (page > 0 && page < last) {
            go_to(page + 1);
        } else {
            cue_ = MenuCue::refused;
        }
        return SetupResult::stay;
    case MenuInput::previous_tab:
    case MenuInput::back:
        if (page > 0) {
            go_to(page - 1);
            return SetupResult::stay;
        }
        if (input == MenuInput::back && start_ == SetupStart::again) {
            cue_ = MenuCue::back;
            return take(SetupOwn::cancel);
        }
        cue_ = MenuCue::refused;
        return SetupResult::stay;
    default:
        break;
    }
    model_.press(input);
    SetupOwn own = pending_;
    pending_ = SetupOwn::none;
    return own == SetupOwn::none ? SetupResult::stay : take(own);
}

void SetupFlow::point_at_row(int row) {
    model_.point_at_row(row);
}

SetupResult SetupFlow::click_row(int row) {
    model_.click_row(row);
    SetupOwn own = pending_;
    pending_ = SetupOwn::none;
    return own == SetupOwn::none ? SetupResult::stay : take(own);
}

void SetupFlow::click_step(int row, int delta) {
    model_.click_step(row, delta);
}

void SetupFlow::click_choice(int choice) {
    model_.click_choice(choice);
}

void SetupFlow::click_page(int page) {
    if (page >= 0 && page <= reached_ && page != model_.tab()) {
        go_to(page);
    }
}

void SetupFlow::show_page(int page) {
    reached_ = std::max(reached_, page);
    go_to(page);
    cue_ = MenuCue::none;
}

MenuCue SetupFlow::take_cue() {
    MenuCue model = model_.take_cue();
    MenuCue own = cue_;
    cue_ = MenuCue::none;
    return own != MenuCue::none ? own : model;
}

} // namespace ui
