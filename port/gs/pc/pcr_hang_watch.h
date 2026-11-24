// The watch for a GPU that has stopped answering (docs/port/PC_OPTIMIZATION.md, "The GPU hang").
//
// After a hang the driver resets the card and Granite waits for ever for results that will not come (kf4-pc in
// vkGetQueryPoolResults, the pump behind its lock): the game stays on the screen frozen and has to be killed. The
// watch's thread ("kf4-pc-watch") looks once a second at the waits for the GPU (HangNotes). One that has lasted
// hang_seconds of its own looks (so a process stopped in a debugger, or a machine asleep, is not a hang) is a hang:
// it writes what the renderer last gave the GPU to <folder>/gpu_hang_<time>.txt and to stderr, and ends the process
// (nothing can be drawn again: the device is lost) with HANG_EXIT_CODE. KF4_PC_HANG_WATCH=0: no watch (pcr.cpp).
#pragma once

#include <memory>
#include <string>

#include "pcr_hang_notes.h"
#include "port/exit_codes.h"

namespace pcr {

constexpr int HANG_SECONDS = 15; // (the renderer's)
constexpr int HANG_EXIT_CODE = PORT_EXIT_GPU_LOST;

// The report's text: the wait, and the notes, newest last, each with how long before `now_s` it was made
std::string hang_report(const HangNotes &notes, const char *where, double waited_s, double now_s);

class HangWatch {
public:
    // folder: where the report goes (the data folder); notes outlive the watch
    HangWatch(const HangNotes &notes, std::string folder, int hang_seconds);
    ~HangWatch();
    HangWatch(const HangWatch &) = delete;
    HangWatch &operator=(const HangWatch &) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// The steady clock the notes and the watch use, in seconds
double hang_clock_s();

} // namespace pcr
