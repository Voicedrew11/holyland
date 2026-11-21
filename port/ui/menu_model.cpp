// The player menu's moving about (port/ui/menu_model.hpp)
#include "menu_model.hpp"

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

void MenuModel::step_row(int delta) {
    if (row_ < 0) {
        return;
    }
    RowShape shape = rows_.shape(tab_, row_);
    switch (shape.kind) {
    case RowKind::choice: {
        int next = (shape.current < 0 ? (delta > 0 ? -1 : shape.choice_count) : shape.current) + delta;
        if (next >= 0 && next < shape.choice_count) {
            rows_.choose(tab_, row_, next);
        }
        break;
    }
    case RowKind::toggle:
        rows_.choose(tab_, row_, delta > 0 ? 1 : 0);
        break;
    case RowKind::number:
        rows_.step(tab_, row_, delta);
        break;
    case RowKind::binding:
        slot_ = slot_ + delta < 0 ? 0 : slot_ + delta >= shape.choice_count ? shape.choice_count - 1 : slot_ + delta;
        break;
    case RowKind::action:
    case RowKind::heading:
        break;
    }
}

void MenuModel::confirm_row() {
    if (row_ < 0) {
        return;
    }
    RowShape shape = rows_.shape(tab_, row_);
    switch (shape.kind) {
    case RowKind::choice:
        if (shape.choice_count > 0) {
            list_open_ = true;
            list_choice_ = shape.current >= 0 ? shape.current : 0;
        }
        break;
    case RowKind::toggle:
        rows_.choose(tab_, row_, shape.current ? 0 : 1);
        break;
    case RowKind::action:
        rows_.activate(tab_, row_);
        break;
    case RowKind::binding:
        rows_.listen(tab_, row_, slot_);
        break;
    case RowKind::number:
    case RowKind::heading:
        break;
    }
}

void MenuModel::clear_row() {
    if (row_ >= 0 && rows_.shape(tab_, row_).kind == RowKind::binding) {
        rows_.clear(tab_, row_, slot_);
    }
}

MenuResult MenuModel::press(MenuInput input) {
    settle();
    if (list_open_) {
        int count = rows_.shape(tab_, row_).choice_count;
        switch (input) {
        case MenuInput::up:
            list_choice_ = list_choice_ > 0 ? list_choice_ - 1 : 0;
            break;
        case MenuInput::down:
            list_choice_ = list_choice_ + 1 < count ? list_choice_ + 1 : count - 1;
            break;
        case MenuInput::confirm:
            list_open_ = false;
            rows_.choose(tab_, row_, list_choice_);
            break;
        case MenuInput::back:
            list_open_ = false;
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
        row_ = next >= 0 ? next : row_;
        break;
    }
    case MenuInput::left:
    case MenuInput::right:
        step_row(input == MenuInput::left ? -1 : 1);
        break;
    case MenuInput::confirm:
        confirm_row();
        break;
    case MenuInput::clear:
        clear_row();
        break;
    case MenuInput::back:
        return MenuResult::close;
    case MenuInput::next_tab:
    case MenuInput::previous_tab: {
        int count = rows_.tab_count();
        open_at((tab_ + (input == MenuInput::next_tab ? 1 : count - 1)) % count);
        break;
    }
    }
    settle();
    return MenuResult::stay;
}

void MenuModel::point_at_row(int row) {
    settle();
    if (!list_open_ && usable(row)) {
        row_ = row;
    }
}

void MenuModel::click_row(int row) {
    settle();
    if (list_open_) {
        list_open_ = false; // a click outside the list closes it
        return;
    }
    if (usable(row)) {
        row_ = row;
        confirm_row();
    }
    settle();
}

void MenuModel::click_step(int row, int delta) {
    settle();
    list_open_ = false;
    if (usable(row)) {
        row_ = row;
        step_row(delta);
    }
    settle();
}

void MenuModel::click_choice(int choice) {
    settle();
    if (list_open_ && choice >= 0 && choice < rows_.shape(tab_, row_).choice_count) {
        list_open_ = false;
        rows_.choose(tab_, row_, choice);
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
    }
    settle();
}

void MenuModel::click_tab(int tab) {
    if (tab >= 0 && tab < rows_.tab_count() && tab != tab_) {
        open_at(tab);
    }
}

} // namespace ui
