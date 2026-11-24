// What a problem report says about this PC and this run (port/report/), as text a fact a line. The game's thread: SDL's
// displays, windows and controllers are asked. Not yet redacted: problem_report.cpp redacts all of it with
// private_words before it goes in the zip or the link.
#pragma once

#include <string>
#include <vector>

#include "report_text.hpp"

namespace report {

// system.txt: the build, the system and the session, the CPU, memory (the PC's and the game's), the GPU and its
// driver, each display, the windows, sound, controllers and the free space where the data folder is
std::string system_facts();

// The report form's Details: the few lines of system_facts a reader wants before opening the zip
std::string system_summary();

// game.txt: how long the game has run, its vblanks, and the options that change what it does
std::string run_facts();

// environment.txt: the command line, and every variable set that changes the game, SDL or the GPU's driver
std::string environment_facts();

// The words that would say who the player is: the home folder, the user's name, the computer's name
std::vector<Redaction> private_words();

} // namespace report
