// What a problem report says about this PC and this run (port/report/), as text a fact a line. The game's thread: SDL's
// displays, windows and controllers are asked. Not yet redacted: problem_report.cpp redacts all of it with
// its bundle (report_bundle.hpp) before it goes in the zip or the link.
#pragma once

#include <string>

namespace report {

// system.txt: the build, the PC's facts (pc_facts.hpp), the game's memory, the GPU and its driver, each display, the
// windows, sound and controllers
std::string system_facts();

// game.txt: how long the game has run, its vblanks, and the options that change what it does
std::string run_facts();

} // namespace report
