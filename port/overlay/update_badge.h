// The update notice in the overlay (docs/port/LAUNCHER.md, "Updates"): what the launcher found (update.json)
// shown as a short note when the game starts, a gold "Update available!" in the F1 menu bar and a window
// with the release's page. The overlay never asks the network itself. Main thread.
#pragma once

// Reads update.json and launcher.json again when due (every few seconds for the first minute: the
// launcher's check may finish after the game starts). Returns true once, when a notice is newly due.
bool update_badge_poll(unsigned now_ms);

// The note shown when the game starts ("Update available: v0.2.0, F1 for details")
const char *update_badge_note();

// In the menu bar: the gold button, when a notice is due
void update_badge_menu_item();

// The release's window, when the button opened it
void update_badge_window();

// "Holyland 0.2.0" or "Holyland (developer's build abc1234)"
const char *update_badge_version_line();

// For the player menu (port/ui/settings_rows.cpp): an update is due and not put off; what it says ("Holyland v0.3.0 is
// out. You have 0.2.0."); its release page opened in the browser; this version skipped for good
bool update_badge_due();
const char *update_badge_summary();
void update_badge_open_page();
void update_badge_skip();
