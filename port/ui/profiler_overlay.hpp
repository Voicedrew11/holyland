// The profiler (Shift+P; docs/port/PROFILING.md): an RmlUi overlay over the whole window that shows everything at
// once, with no tabs or scrolling, while the game keeps the keyboard, mouse and controller. Its document is
// port/ui/res/profiler.rml (look: profiler.rcss), in the player menu's context (player_ui.cpp), under the menu.
//
// It shows the pictures shown against the screen's rate, the PC under the game (CPU speed, memory, the clock's cost),
// the pipelines a draw waited for; a bar a picture; one picture's timeline (every lane's zones, nested); each lane's
// and each zone's time a picture over the last seconds; the spikes; and the profile run's functions and GPU work.
// Its own keys, all with Shift and only while it shows (the letters the game leaves free):
//
//   Shift+P  show / hide            Shift+O  hold what is shown still (the record goes on)
//   Shift+U  the timeline's picture: the newest, or the slowest of the last seconds
//   Shift+Y  the GPU's detail: by kind of work, by shader state, each draw
//   Shift+H  the profile run: start / stop      Shift+B  save it     Shift+N  copy everything as text
//
// While shown the frame profiler records (port_frame_profiler_want). All on the thread that draws the player menu.
#pragma once

#include <memory>

union SDL_Event;

namespace Rml {
class Context;
}

namespace ui {

class ProfilerOverlay {
public:
    ProfilerOverlay();
    ~ProfilerOverlay();
    ProfilerOverlay(const ProfilerOverlay &) = delete;
    ProfilerOverlay &operator=(const ProfilerOverlay &) = delete;

    // Once, after Rml::Initialise and the fonts: its document loaded into context, hidden. False if it did not load.
    bool load(Rml::Context &context);
    // Its keys (above): true when the event was one of them, which nothing else may see
    bool event(const SDL_Event &event);
    void show(bool shown);
    bool shown() const;
    // Each picture it shows, before the context's Update: what is shown read again (a few times a second) and the
    // document's parts written where they changed
    void update();

    struct State; // (profiler_overlay.cpp's own)

private:
    std::unique_ptr<State> state_;
};

} // namespace ui
