// The first-start setup's page, written as RML into the player menu's document (port/ui/res/menu.rml, with
// body.setup in port/ui/res/kf4.rcss): the steps down the sidebar, the page's title and its few words, its rows (the
// rows_rml.hpp the menu uses: the Welcome's cards are its action rows), and over the focused row's help, the page's
// preview drawn from the settings as they are now (port/ui/setup_pages.hpp's PreviewKind). The game's thread.
#pragma once

#include <string>

#include "pad_glyphs.hpp"
#include "setup_model.hpp"

namespace ui {

// the steps, the current one lit and those reached marked (data-page for the pointer)
std::string setup_steps_rml(const SetupFlow &flow);
// the page's title, then its few words
std::string setup_heading_rml(const SetupFlow &flow);
std::string setup_rows_rml(const SetupFlow &flow, PadFamily family);
// the preview, then the focused row's help. pad: the controller was used last (its pictures on the Controls page)
std::string setup_help_rml(const SetupFlow &flow, PadFamily family);

} // namespace ui
