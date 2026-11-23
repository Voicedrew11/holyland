// A page's rows and the focused row's help, written as RML from the rows and the model: the player menu's
// (port/ui/player_ui.cpp) and the first-start setup's (port/ui/setup_view.hpp). The style sheet (port/ui/res/kf4.rcss)
// gives the elements their look. The game's thread.
#pragma once

#include <string>
#include <string_view>

#include "menu_model.hpp"
#include "pad_glyphs.hpp"
#include "settings_rows.hpp"

namespace ui {

// text into RML, with & < > " escaped
void rml_escaped(std::string &out, std::string_view text);

// The model's tab's rows: headings, each row's label and value (the focused one's arrows, a number's bar, an open
// list's choices, dimmed choices marked), a binding's slots (the controller's slot in family's pictures)
std::string rows_rml(const RowSource &rows, const MenuModel &model, PadFamily family);
// The focused row's label, help, and notes (the command line's, changed from the default, the row's own)
std::string help_rml(const RowSource &rows, const MenuModel &model);

} // namespace ui
