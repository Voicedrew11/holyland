// The profiler's window (Shift+P, port/overlay/profiler.cpp; docs/port/PROFILING.md): the pictures shown against the
// screen's, the pipelines a draw waited for, then the frame profiler's record (port/include/port/frame_profiler.h)
// picture by picture: a bar a picture (its length, and the GPU's time in it), the spikes, the picked picture's
// timeline (every thread's zones and the GPU's, nested, on one clock) and a table of zones (self, inclusive, calls)
// for that picture or averaged over the last seconds; last, the profile run's functions (profile_run_view.h).
//
// draw runs on the thread that shows the picture (inside the overlay's ImGui frame). The record is copied into the
// window's own a few times a second, never waited for (zones::with_record's try).
#ifndef PORT_OVERLAY_FRAMES_VIEW_H
#define PORT_OVERLAY_FRAMES_VIEW_H

// (any thread) Shown or not: while shown, the frames are wanted (recorded)
void frames_view_show(bool show);
// (inside the overlay's ImGui frame) the window; it has the mouse while shown (profiler.h)
void frames_view_draw();

#endif
