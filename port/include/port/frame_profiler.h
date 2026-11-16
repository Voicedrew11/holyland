/* The frame profiler's owner (docs/port/PROFILING.md, "Frames"): a thread of its own ("kf4-profiler") that reads
   the zones' rings (port/include/port/zones.h) into the frames (zone_record.hpp) twenty times a second while anyone
   wants them, and what is asked of it from the environment:

     KF4_PROFILE=1            record from the start; a summary on stderr every 5 s
     KF4_PROFILE_OUT=FILE     every frame's zones as CSV rows (frame,start_ms,lane,zone,kind,self_ms,incl_ms,calls),
                              for tools/sampler/frames_report.py
     KF4_PROFILE_SPIKE=MS     a line on stderr for each frame longer than MS, with its largest zones
     KF4_GPU_DETAIL=1|2       the GPU's lanes by shader state (1) or each draw (2), not only by kind of work

   Shift+P's "Frames" (port/overlay/frames_view.cpp) wants it while it is open.
   port/platform/frame_profiler.cpp, kf4_zone_record. */
#ifndef PORT_FRAME_PROFILER_H
#define PORT_FRAME_PROFILER_H

#ifdef __cplusplus
extern "C" {
#endif

/* At start-up (main_host.c): what the environment asks for started */
void port_frame_profiler_start(void);
/* At exit: the CSV written out, the thread stopped */
void port_frame_profiler_stop(void);
/* The frames wanted (+1) or not (-1): the panel's. Recording and the thread follow. */
void port_frame_profiler_want(int more);

#ifdef __cplusplus
}

#include <functional>

namespace zones {
class Record;
// f with the record, under its lock, if the lock is free: false when the thread is working the record (the panel's
// reading, from the thread that shows the picture: it tries again at its next picture rather than wait)
bool with_record(const std::function<void(Record &)> &f);
} // namespace zones
#endif

#endif
