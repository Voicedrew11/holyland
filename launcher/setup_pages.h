// The setup's pages (setup_flow.h). A new page, such as an option to pick before the first game, is a new make_
// function here and a line in the session's list of pages (session.cpp, Session::setup_pages). Main thread.
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "setup_flow.h"

namespace launcher {

// What the disc page needs from the session
class DiscKeeper {
public:
    virtual ~DiscKeeper() = default;
    // checks the disc (disc_check.h) and keeps it when it is King's Field IV; false when not, and why_not() says why
    virtual bool keep_disc(const std::filesystem::path &disc) = 0;
    // why the last disc offered (or the one chosen at an earlier start) was refused; "" when none was
    virtual const std::string &why_not() const = 0;
    // the system's chooser the launcher used before (Platform::pick_disc: kdialog or zenity, blocking), for when
    // the window's own cannot open; nullopt when the player chose nothing or there is none
    virtual std::optional<std::filesystem::path> pick_disc_the_old_way() = 0;
};

// The disc: dropped on the window, or chosen by clicking the drop area (the system's own file chooser), or a path
// typed in. Answers next once the keeper has kept one.
std::unique_ptr<SetupPage> make_disc_page(DiscKeeper &keeper);

} // namespace launcher
