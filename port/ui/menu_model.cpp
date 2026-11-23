// The player menu's moving about (port/ui/menu_model.hpp)
#include "menu_model.hpp"

#include <algorithm>

namespace ui {

bool MenuModel::usable(int row) const {
    if (row < 0 || row >= rows_.row_count(tab_)) {
        return false;
    }
    RowShape shape = rows_.shape(tab_, row);
    return shape.enabled && shape.kind != RowKind::heading;
}

int MenuModel::first_usable() const {
    return usable_from(-1, 1);
}

// The first usable row after (or before) row, going that way; -1 when there is none
int MenuModel::usable_from(int row, int direction) const {
    for (int at = row + direction; at >= 0 && at < rows_.row_count(tab_); at += direction) {
        if (usable(at)) {
            return at;
        }
    }
    return -1;
}

void MenuModel::settle() {
    if (tab_ >= rows_.tab_count()) {
        tab_ = 0;
    }
    if (!usable(row_)) {
        // the row went (hidden or disabled): the next one down, else the one before
        int next = usable_from(row_, 1);
        row_ = next >= 0 ? next : usable_from(row_ < rows_.row_count(tab_) ? row_ : rows_.row_count(tab_), -1);
    }
    if (row_ >= 0) {
        RowShape shape = rows_.shape(tab_, row_);
        if (shape.kind == RowKind::binding && slot_ >= shape.choice_count) {
            slot_ = shape.choice_count > 0 ? shape.choice_count - 1 : 0;
        }
    }
    if (list_open_) {
        RowShape shape = row_ >= 0 ? rows_.shape(tab_, row_) : RowShape{};
        if (row_ < 0 || shape.kind != RowKind::choice || shape.choice_count <= 0) {
            list_open_ = false;
        } else if (list_choice_ >= shape.choice_count) {
            list_choice_ = shape.choice_count - 1;
        }
    }
}

void MenuModel::open_at(int tab) {
    tab_ = tab >= 0 && tab < rows_.tab_count() ? tab : 0;
    list_open_ = false;
    row_ = first_usable();
}

// (the last cue made wins: Right then Clear on a slot is the clear's)
void MenuModel::cue(MenuCue cue) {
    if (cue != MenuCue::none) {
        cue_ = cue;
    }
}

MenuCue MenuModel::take_cue() {
    MenuCue taken = cue_;
    cue_ = MenuCue::none;
    return taken;
}

bool MenuModel::step_row(int delta) {
    if (row_ < 0) {
        return false;
    }
    RowShape shape = rows_.shape(tab_, row_);
    switch (shape.kind) {
    case RowKind::choice: {
        int next = (shape.current < 0 ? (delta > 0 ? -1 : shape.choice_count) : shape.current) + delta;
        while (next >= 0 && next < shape.choice_count && !rows_.choice_enabled(tab_, row_, next)) {
            next += delta; // (a dimmed choice is passed over)
        }
        if (next < 0 || next >= shape.choice_count) {
            return false;
        }
        rows_.choose(tab_, row_, next);
        return true;
    }
    case RowKind::toggle: {
        int wanted = delta > 0 ? 1 : 0;
        if (shape.current == wanted) {
            return false;
        }
        rows_.choose(tab_, row_, wanted);
        return true;
    }
    case RowKind::number:
        rows_.step(tab_, row_, delta);
        return true;
    case RowKind::binding: {
        int next = std::clamp(slot_ + delta, 0, shape.choice_count > 0 ? shape.choice_count - 1 : 0);
        bool moved = next != slot_;
        slot_ = next;
        return moved;
    }
    case RowKind::action:
    case RowKind::heading:
        break;
    }
    return false;
}

bool MenuModel::confirm_row() {
    if (row_ < 0) {
        return false;
    }
    RowShape shape = rows_.shape(tab_, row_);
    switch (shape.kind) {
    case RowKind::choice:
        if (shape.choice_count <= 0) {
            return false;
        }
        list_open_ = true;
        list_choice_ = shape.current >= 0 ? shape.current : 0;
        return true;
    case RowKind::toggle:
        rows_.choose(tab_, row_, shape.current ? 0 : 1);
        return true;
    case RowKind::action:
        rows_.activate(tab_, row_);
        return true;
    case RowKind::binding:
        rows_.listen(tab_, row_, slot_);
        return true;
    case RowKind::number:
    case RowKind::heading:
        break;
    }
    return false;
}

bool MenuModel::clear_row() {
    if (row_ < 0 || rows_.shape(tab_, row_).kind != RowKind::binding) {
        return false;
    }
    rows_.clear(tab_, row_, slot_);
    return true;
}

MenuResult MenuModel::press(MenuInput input) {
    settle();
    if (list_open_) {
        int count = rows_.shape(tab_, row_).choice_count;
        int was = list_choice_;
        switch (input) {
        case MenuInput::up:
            list_choice_ = list_choice_ > 0 ? list_choice_ - 1 : 0;
            cue(list_choice_ != was ? MenuCue::move : MenuCue::none);
            break;
        case MenuInput::down:
            list_choice_ = list_choice_ + 1 < count ? list_choice_ + 1 : count - 1;
            cue(list_choice_ != was ? MenuCue::move : MenuCue::none);
            break;
        case MenuInput::confirm:
            if (!rows_.choice_enabled(tab_, row_, list_choice_)) {
                cue(MenuCue::refused); // (a dimmed choice: the list stays open)
                break;
            }
            list_open_ = false;
            rows_.choose(tab_, row_, list_choice_);
            cue(MenuCue::confirm);
            break;
        case MenuInput::back:
            list_open_ = false;
            cue(MenuCue::back);
            break;
        case MenuInput::next_tab:
        case MenuInput::previous_tab:
            list_open_ = false;
            return press(input);
        case MenuInput::left:
        case MenuInput::right:
        case MenuInput::clear:
            break;
        }
        settle();
        return MenuResult::stay;
    }
    switch (input) {
    case MenuInput::up:
    case MenuInput::down: {
        int next = row_ < 0 ? first_usable() : usable_from(row_, input == MenuInput::up ? -1 : 1);
        cue(next >= 0 && next != row_ ? MenuCue::move : MenuCue::none);
        row_ = next >= 0 ? next : row_;
        break;
    }
    case MenuInput::left:
    case MenuInput::right:
        cue(step_row(input == MenuInput::left ? -1 : 1) ? MenuCue::move : MenuCue::none);
        break;
    case MenuInput::confirm:
        cue(confirm_row() ? MenuCue::confirm : MenuCue::refused);
        break;
    case MenuInput::clear:
        cue(clear_row() ? MenuCue::back : MenuCue::none);
        break;
    case MenuInput::back:
        cue(MenuCue::back);
        return MenuResult::close;
    case MenuInput::next_tab:
    case MenuInput::previous_tab: {
        int count = rows_.tab_count();
        open_at((tab_ + (input == MenuInput::next_tab ? 1 : count - 1)) % count);
        cue(count > 1 ? MenuCue::move : MenuCue::none);
        break;
    }
    }
    settle();
    return MenuResult::stay;
}

void MenuModel::point_at_row(int row) {
    settle();
    if (!list_open_ && usable(row) && row != row_) {
        row_ = row;
        cue(MenuCue::move);
    }
}

void MenuModel::click_row(int row) {
    settle();
    if (list_open_) {
        list_open_ = false; // a click outside the list closes it
        cue(MenuCue::back);
        return;
    }
    if (usable(row)) {
        row_ = row;
        cue(confirm_row() ? MenuCue::confirm : MenuCue::refused);
    } else if (row >= 0 && row < rows_.row_count(tab_) && !rows_.shape(tab_, row).enabled) {
        cue(MenuCue::refused);
    }
    settle();
}

void MenuModel::click_step(int row, int delta) {
    settle();
    list_open_ = false;
    if (usable(row)) {
        row_ = row;
        cue(step_row(delta) ? MenuCue::move : MenuCue::none);
    }
    settle();
}

void MenuModel::click_choice(int choice) {
    settle();
    if (list_open_ && choice >= 0 && choice < rows_.shape(tab_, row_).choice_count) {
        if (!rows_.choice_enabled(tab_, row_, choice)) {
            cue(MenuCue::refused);
            return;
        }
        list_open_ = false;
        rows_.choose(tab_, row_, choice);
        cue(MenuCue::confirm);
    }
    settle();
}

void MenuModel::click_slot(int row, int slot) {
    settle();
    list_open_ = false;
    if (usable(row) && rows_.shape(tab_, row).kind == RowKind::binding && slot >= 0 &&
        slot < rows_.shape(tab_, row).choice_count) {
        row_ = row;
        slot_ = slot;
        rows_.listen(tab_, row_, slot_);
        cue(MenuCue::confirm);
    }
    settle();
}

void MenuModel::click_tab(int tab) {
    if (tab >= 0 && tab < rows_.tab_count() && tab != tab_) {
        open_at(tab);
        cue(MenuCue::move);
    }
}

} // namespace ui
