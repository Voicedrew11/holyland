// The profiler panel's "Functions and GPU" section (port/overlay/profiler.cpp, Shift+P): a profile run's Start/Stop
// (port/include/port/profile_run.h), each thread's functions by self and inclusive share of its CPU time, and the
// GPU's time a picture by kind of work and, with detail on, by shader state. Save writes the run for
// tools/sampler/stacks.py; Copy puts the tables on the clipboard.
#ifndef PORT_OVERLAY_PROFILE_RUN_VIEW_H
#define PORT_OVERLAY_PROFILE_RUN_VIEW_H

// (the game's thread, twice a second while the panel is shown) the run's latest report taken in
void profile_run_view_step();
// (inside the panel's ImGui window) the section; overlay_open: the mouse is the overlay's, the buttons work
void profile_run_view_draw(bool overlay_open);

#endif
