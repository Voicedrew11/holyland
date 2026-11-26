// The player menu's Mods page (port/ui/settings_rows.cpp's "mods" tab): each mod in the mods folder with its name,
// version, author and description, whether it is on, and the settings its mod.json declares (port/include/port/
// mods.h). The rows are made when the menu starts, from the mod list; a row's functions are made for its place in the
// table (row_get<N> ...), as the menu's rows hold plain function pointers.
#include <array>
#include <cstdio>
#include <string>
#include <utility>

#include "settings_rows.hpp"

extern "C" {
#include "port/mod_api.h"
#include "port/mods.h"
#include "port/settings.h"
}

namespace ui {

namespace {

constexpr size_t MOD_ROWS = 160;

enum class Part { unused, heading, on, setting, none_installed };

struct Slot {
    Part part = Part::unused;
    int mod = -1;
    int setting = -1;
    std::string label, help;
    std::array<Choice, PORT_MOD_CHOICES> choices{};
};

std::array<Slot, MOD_ROWS> slots;

PortMod *mod_of(size_t n) {
    return port_mods_get(slots[n].mod);
}

int get_at(size_t n) {
    PortMod *mod = mod_of(n);
    if (mod == nullptr) {
        return 0;
    }
    return slots[n].part == Part::on ? mod->on : mod->settings[slots[n].setting].value;
}

void set_at(size_t n, int value) {
    PortMod *mod = mod_of(n);
    if (mod == nullptr) {
        return;
    }
    if (slots[n].part == Part::on) {
        mod->on = value != 0;
    } else {
        PortModSetting &set = mod->settings[slots[n].setting];
        set.value = value < set.min ? set.min : value > set.max ? set.max : value;
    }
    port_settings_changed();
}

bool shown_at(size_t n) {
    PortMod *mod = mod_of(n);
    switch (slots[n].part) {
    case Part::heading:
    case Part::on:
    case Part::none_installed:
        return true;
    case Part::setting:
        return mod != nullptr && mod->applied; // (its settings only once its plugin or data is in effect)
    default:
        return false;
    }
}

/* The game functions this mod changes that other mods change too (port_mod_hooks_shared), as the page says it */
std::string shared_hooks(const PortMod *mod) {
    PortModHookShare shares[8];
    int count = mod->applied ? port_mod_hooks_shared(mod->id, shares, 8) : 0;
    std::string text;

    for (int i = 0; i < count; i++) {
        std::string names, others = shares[i].others;
        size_t at = 0;

        while (at < others.size()) {
            size_t end = others.find('\n', at);
            std::string id = others.substr(at, end == std::string::npos ? std::string::npos : end - at);
            const PortMod *other = port_mods_by_id(id.c_str());

            names += (names.empty() ? "" : ", ") + std::string(other != nullptr ? other->name : id.c_str());
            at = end == std::string::npos ? others.size() : end + 1;
        }
        text += "\nChanges the same part of the game as " + names + " (" + shares[i].function +
                "). They usually work together; if something from one of them stops working, try turning the other off.";
    }
    return text;
}

const char *note_at(size_t n) {
    PortMod *mod = mod_of(n);
    if (slots[n].part == Part::on && mod != nullptr && mod->on != mod->applied) {
        return mod->on ? "On from the next start of Holyland." : "Off from the next start of Holyland.";
    }
    return "";
}

template <size_t N> int row_get() {
    return get_at(N);
}
template <size_t N> void row_set(int value) {
    set_at(N, value);
}
template <size_t N> bool row_shown() {
    return shown_at(N);
}
template <size_t N> const char *row_note() {
    return note_at(N);
}

template <size_t... N> std::array<SettingRow, MOD_ROWS> blank_rows(std::index_sequence<N...>) {
    return {SettingRow{"", "", RowKind::heading, {}, {}, row_get<N>, row_set<N>, nullptr, nullptr, row_shown<N>, nullptr,
                       nullptr, row_note<N>}...};
}

const Choice off_on[] = {{"Off", 0}, {"On", 1}};

} // namespace

std::array<SettingRow, MOD_ROWS> mod_rows_table = blank_rows(std::make_index_sequence<MOD_ROWS>());

void fill_mod_rows() {
    size_t at = 0;
    auto add = [&](Part part, int mod, int setting) -> Slot * {
        if (at >= MOD_ROWS) {
            return nullptr;
        }
        Slot &slot = slots[at];
        slot = Slot();
        slot.part = part, slot.mod = mod, slot.setting = setting;
        at++;
        return &slot;
    };
    if (port_mods_count() == 0) {
        if (Slot *slot = add(Part::none_installed, -1, -1)) {
            slot->label = "No mods installed";
        }
    }
    for (int m = 0; m < port_mods_count(); m++) {
        PortMod *mod = port_mods_get(m);
        std::string about = mod->description;
        if (mod->author[0]) {
            about += std::string(about.empty() ? "" : "\n") + "By " + mod->author + ".";
        }
        if (mod->has_plugin) {
            about += std::string(about.empty() ? "" : "\n") +
                     "Contains code: it runs its own program inside Holyland. Turn on only mods you trust.";
            if (!mod->plugin[0]) {
                about += " It has no plugin for this system, so only its files and text are used.";
            }
        }
        about += shared_hooks(mod);
        about += std::string(about.empty() ? "" : "\n") + "On and off take effect at the next start.";
        if (Slot *slot = add(Part::heading, m, -1)) {
            slot->label = std::string(mod->name) + (mod->version[0] ? std::string("  ") + mod->version : "");
        }
        if (Slot *slot = add(Part::on, m, -1)) {
            slot->label = "On";
            slot->help = about;
        }
        for (int k = 0; k < mod->setting_count; k++) {
            if (Slot *slot = add(Part::setting, m, k)) {
                const PortModSetting &set = mod->settings[k];
                slot->label = set.label;
                slot->help = set.help;
                for (int c = 0; c < set.choice_count; c++) {
                    slot->choices[size_t(c)] = {set.choices[c], c};
                }
            }
        }
    }
    // the rows from the slots (strings and choices kept in the slots, which stay put)
    for (size_t n = 0; n < MOD_ROWS; n++) {
        SettingRow &row = mod_rows_table[n];
        Slot &slot = slots[n];
        row.label = slot.label.c_str();
        row.help = slot.help.c_str();
        row.choices = {};
        row.range = {};
        switch (slot.part) {
        case Part::on:
            row.kind = RowKind::toggle;
            row.choices = off_on;
            break;
        case Part::setting: {
            const PortModSetting &set = mod_of(n)->settings[slot.setting];
            if (set.type == PORT_MOD_TOGGLE) {
                row.kind = RowKind::toggle;
                row.choices = off_on;
            } else if (set.type == PORT_MOD_CHOICE) {
                row.kind = RowKind::choice;
                row.choices = std::span<const Choice>(slot.choices.data(), size_t(set.choice_count));
            } else {
                row.kind = RowKind::number;
                row.range = {set.min, set.max, set.step};
            }
            break;
        }
        default:
            row.kind = RowKind::heading;
            break;
        }
    }
}

} // namespace ui
