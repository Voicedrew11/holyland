// A page's rows and help as RML (port/ui/rows_rml.hpp), moved from port/ui/player_ui.cpp for the setup to share
#include "rows_rml.hpp"

#include <cstdio>
#include <optional>

extern "C" {
#include "port/controls.h"
}

namespace ui {

void rml_escaped(std::string &out, std::string_view text) {
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
}

namespace {

// A binding's slots: the chosen one marked on the focused row, the one waiting for a key lit
void slots_rml(std::string &out, const RowSource &rows, const MenuModel &model, int tab, int row, bool focused,
               PadFamily family) {
    for (int slot = 0; slot < rows.shape(tab, row).choice_count; slot++) {
        std::string text = rows.slot_text(tab, row, slot);
        out += "<slot data-slot=\"" + std::to_string(slot) + "\" class=\"";
        out += focused && slot == model.slot() ? "chosen " : "";
        out += rows.listening(tab, row, slot) ? "listening " : "";
        out += text == "-" ? "empty" : "";
        out += "\">";
        std::optional<PadButton> button;
        if (!rows.listening(tab, row, slot)) {
            button = pad_button_of_binding(port_controls_binding(rows.row(tab, row).action, slot));
        }
        if (button) {
            out += pad_glyph_rml(family, *button);
        } else {
            rml_escaped(out, text);
        }
        out += "</slot>";
    }
}

// A row's value: the arrows either side on the focused row (dim at the ends), a number's bar under it
void value_rml(std::string &out, const RowSource &rows, int tab, int row, const RowShape &shape, bool focused) {
    const SettingRow &r = rows.row(tab, row);
    std::string text = rows.value_text(tab, row);
    out += shape.kind == RowKind::toggle && shape.current == 0 ? "<value class=\"off\">" : "<value>";
    if (r.overridden != nullptr && r.overridden()) {
        out += "<note>command line</note>";
    }
    bool arrows = focused && shape.enabled && shape.kind != RowKind::action;
    bool first = false, last = false;
    if (shape.kind == RowKind::choice || shape.kind == RowKind::toggle) {
        // (an end is the last choice that can be chosen that way)
        first = last = true;
        for (int c = 0; c < shape.choice_count; c++) {
            if (rows.choice_enabled(tab, row, c)) {
                first = first && !(c < shape.current);
                last = last && !(c > shape.current);
            }
        }
        if (shape.current < 0) {
            first = last = false;
        }
    } else if (shape.kind == RowKind::number) {
        float f = rows.number_fraction(tab, row);
        first = f <= 0.0f;
        last = f >= 1.0f;
    }
    if (shape.kind == RowKind::number) {
        char width[32];
        std::snprintf(width, sizeof(width), "%.1f%%", double(rows.number_fraction(tab, row)) * 100.0);
        out += std::string("<bar><fill style=\"width: ") + width + "\"></fill></bar>";
    }
    if (arrows) {
        out += first ? "<arrow class=\"end\">&lt;</arrow>" : "<arrow data-step=\"-1\">&lt;</arrow>";
    }
    rml_escaped(out, text);
    if (arrows) {
        out += last ? "<arrow class=\"end\">&gt;</arrow>" : "<arrow data-step=\"1\">&gt;</arrow>";
    }
    out += "</value>";
}

} // namespace

std::string rows_rml(const RowSource &rows, const MenuModel &model, PadFamily family) {
    std::string out;
    int tab = model.tab();
    for (int row = 0; row < rows.row_count(tab); row++) {
        RowShape shape = rows.shape(tab, row);
        const SettingRow &r = rows.row(tab, row);
        if (shape.kind == RowKind::heading) {
            out += row == 0 ? "<row class=\"heading first\"><label>" : "<row class=\"heading\"><label>";
            rml_escaped(out, r.label);
            out += "</label></row>";
            continue;
        }
        bool focused = row == model.row();
        bool open = focused && model.list_open();
        out += "<row id=\"row-" + std::to_string(row) + "\" data-row=\"" + std::to_string(row) + "\" class=\"";
        out += focused ? "focused " : "";
        out += shape.enabled ? "" : "disabled ";
        out += shape.kind == RowKind::action ? "action " : "";
        out += shape.kind == RowKind::binding ? "binding " : "";
        out += rows.changed_from_default(tab, row) ? "changed " : "";
        out += rows.row_classes(tab, row);
        out += open ? " open" : "";
        out += "\"><label>";
        rml_escaped(out, r.label);
        out += "</label>";
        if (shape.kind == RowKind::binding) {
            slots_rml(out, rows, model, tab, row, focused, family);
        } else if (shape.kind == RowKind::action) {
            out += focused && shape.enabled ? "<value>Enter</value>" : "";
        } else {
            value_rml(out, rows, tab, row, shape, focused);
        }
        out += "</row>";
        if (open) {
            out += "<list>";
            for (int c = 0; c < shape.choice_count; c++) {
                out += "<choice data-choice=\"" + std::to_string(c) + "\" class=\"";
                out += c == model.list_choice() ? "highlighted " : "";
                out += c == shape.current ? "chosen " : "";
                out += rows.choice_enabled(tab, row, c) ? "" : "dimmed";
                out += "\">";
                rml_escaped(out, r.choices[size_t(c)].label);
                out += "</choice>";
            }
            out += "</list>";
        }
    }
    return out;
}

std::string help_rml(const RowSource &rows, const MenuModel &model) {
    int tab = model.tab(), row = model.row();
    if (row < 0) {
        return "";
    }
    const SettingRow &r = rows.row(tab, row);
    std::string out = "<h2>";
    rml_escaped(out, r.label);
    out += "</h2>";
    std::string_view help = r.help;
    while (!help.empty()) {
        size_t end = help.find('\n');
        out += "<p>";
        rml_escaped(out, help.substr(0, end));
        out += "</p>";
        help = end == std::string_view::npos ? std::string_view() : help.substr(end + 1);
    }
    if (r.overridden != nullptr && r.overridden()) {
        out += "<p class=\"note\">This run's value was set on the command line. A change here is kept for the next "
               "start.</p>";
    }
    if (rows.changed_from_default(tab, row)) {
        out += "<p class=\"note\">Changed from the default.</p>";
    }
    if (r.note != nullptr && r.note() != nullptr && r.note()[0] != 0) {
        out += "<p class=\"note\">";
        rml_escaped(out, r.note());
        out += "</p>";
    }
    return out;
}

} // namespace ui
