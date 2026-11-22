// The update notice (docs/port/LAUNCHER.md, "Updates"): what the launcher found (update.json), offered in a pop-up
// at the title (port/include/port/update_offer.h) and in the player menu's General tab. The game never asks the
// network itself. Main thread.
#pragma once

// Each frame: what the player chose in the pop-up, if it has closed; and update.json and launcher.json read
// again when due (every few seconds for the first minute: the launcher's check may finish after the game
// starts). Returns true once, when a notice is newly due.
bool update_badge_poll(unsigned now_ms);

// "Holyland 0.2.0" or "Holyland (developer's build abc1234)"
const char *update_badge_version_line();

// For the player menu (port/ui/settings_rows.cpp): an update is due and not put off; what it says ("Holyland v0.3.0 is
// out. You have 0.2.0."); its release page opened in the browser; this version skipped for good
bool update_badge_due();
const char *update_badge_summary();
void update_badge_open_page();
void update_badge_skip();
// the launcher asked to install it (update-request.json) and the game quit; nothing when none is due
void update_badge_update_now();
