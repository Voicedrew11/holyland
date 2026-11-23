// One row of the player menu's settings, whatever holds it (port/ui/settings_rows.hpp): the menu's tabs and the
// first-start setup's pages read, step and describe their rows alike. No settings code: only the row's own functions.
#include "settings_rows.hpp"

#include <cstdio>

extern "C" {
#include "port/controls.h"
}

namespace ui {

int row_choice_index(const SettingRow &row, int value) {
    for (size_t i = 0; i < row.choices.size(); i++) {
        if (row.choices[i].value == value) {
            return int(i);
        }
    }
    return -1;
}

RowShape row_shape(const SettingRow &r) {
    RowShape shape = {r.kind, r.enabled == nullptr || r.enabled(), int(r.choices.size()), 0};
    if ((r.kind == RowKind::choice || r.kind == RowKind::toggle) && r.get != nullptr) {
        shape.current = row_choice_index(r, r.get());
    } else if (r.kind == RowKind::binding) {
        shape.choice_count = port_controls_action_slots(r.action);
    }
    return shape;
}

bool row_choice_enabled(const SettingRow &r, int choice) {
    if (choice < 0 || size_t(choice) >= r.choices.size()) {
        return false;
    }
    bool (*enabled)() = r.choices[size_t(choice)].enabled;
    return enabled == nullptr || enabled();
}

void row_choose(const SettingRow &r, int choice) {
    if (row_choice_enabled(r, choice) && r.set != nullptr) {
        r.set(r.choices[size_t(choice)].value);
    }
}

void row_step(const SettingRow &r, int delta) {
    if (r.kind != RowKind::number || r.set == nullptr || r.get == nullptr || r.range.step <= 0) {
        return;
    }
    // to the next whole step, so a value between steps (from settings.json) lands on one
    int value = r.get();
    int steps = (value - r.range.min) / r.range.step + (delta > 0 ? 1 : ((value - r.range.min) % r.range.step ? 0 : -1));
    int next = r.range.min + steps * r.range.step;
    next = next < r.range.min ? r.range.min : next > r.range.max ? r.range.max : next;
    if (next != value) {
        r.set(next);
    }
}

void row_activate(const SettingRow &r) {
    if (r.act != nullptr) {
        r.act();
    }
}

std::string row_value_text(const SettingRow &r) {
    char text[96] = "";
    switch (r.kind) {
    case RowKind::choice:
    case RowKind::toggle: {
        if (r.get == nullptr) {
            return text;
        }
        // the row's own text when it gives one ("Auto (2x)"), else the chosen choice's label
        int value = r.get();
        if (r.describe != nullptr) {
            r.describe(value, text, sizeof(text));
        }
        int at = row_choice_index(r, value);
        if (text[0] == 0 && at >= 0) {
            return r.choices[size_t(at)].label;
        }
        return text;
    }
    case RowKind::number:
        if (r.get == nullptr) {
            return text;
        }
        if (r.describe != nullptr) {
            r.describe(r.get(), text, sizeof(text));
        } else {
            std::snprintf(text, sizeof(text), "%d", r.get());
        }
        return text;
    case RowKind::action:
    case RowKind::heading:
    case RowKind::binding:
        break;
    }
    return text;
}

float row_number_fraction(const SettingRow &r) {
    if (r.kind != RowKind::number || r.range.max <= r.range.min || r.get == nullptr) {
        return 0.0f;
    }
    float f = float(r.get() - r.range.min) / float(r.range.max - r.range.min);
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

} // namespace ui
