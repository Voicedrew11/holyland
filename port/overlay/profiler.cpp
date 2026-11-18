// The profiler (Shift+P, profiler.h; docs/port/PROFILING.md): one window, drawn by frames_view.cpp. This file is its
// switch: the key, the overlay shot's, and the profile run's report taken in twice a second.
//
// profiler_key and profiler_step run on the game's thread (the window's events, the vblank); profiler_draw on the
// thread that shows the picture.
#include <SDL.h>

#include <atomic>

#include "frames_view.h"
#include "profile_run_view.h"
#include "profiler.h"

extern "C" {
#include "port/platform.h"
}

namespace {

const double STEP_SECONDS = 0.5;

std::atomic<bool> shown{false};
double stepped_s = -1; // (the game's thread)

void set_shown(bool show) {
    shown.store(show);
    frames_view_show(show);
}

} // namespace

bool profiler_key(const SDL_Event &event) {
    if (event.type != SDL_KEYDOWN || event.key.repeat || event.key.keysym.scancode != SDL_SCANCODE_P ||
        !(event.key.keysym.mod & KMOD_SHIFT)) {
        return false;
    }
    set_shown(!shown.load());
    return true;
}

void profiler_show_for_shot(bool show) {
    if (show != shown.load()) {
        set_shown(show);
    }
}

bool profiler_visible() {
    return shown.load();
}

void profiler_step() {
    if (!shown.load()) {
        return;
    }
    double now = port_wall_seconds();
    if (now - stepped_s < STEP_SECONDS) {
        return;
    }
    stepped_s = now;
    profile_run_view_step();
}

void profiler_draw() {
    frames_view_draw();
}
