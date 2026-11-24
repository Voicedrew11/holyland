// What a report says about this PC, a fact a line, without SDL or the game (port/report/): the game's Report an Issue
// (system_facts.hpp adds the GPU, displays and the run) and the launcher's crash report (launcher/crash_report.h)
// both say it. Not redacted: the bundle (report_bundle.hpp) redacts what goes in with private_words. C++17.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "report_text.hpp"

namespace report {

// "System: ...\n" and on Linux "Session: ...\n" (Wayland or X11, the desktop, AppImage, Flatpak, gamescope); Wine
std::string os_facts();
// "CPU: <name> (<threads> threads; AVX2)\n"
std::string cpu_facts();
// "Memory: <total> MB, <free> MB free\n"
std::string memory_facts();
// "<what>: <MB> MB now, <MB> MB at most\n": this process's memory
std::string process_memory_facts(const char *what);
// "Free space in the data folder: <MB> MB\n"
std::string disk_facts(const std::filesystem::path &folder);

// This process's command line
std::string this_command_line();
// environment.txt: the command line given, and every variable set that changes the game, SDL, the Vulkan loader or a
// GPU driver
std::string environment_facts(const std::string &command_line);

// The words that would say who the player is: the home folder, the user's name, the computer's name (not a name the
// system gave every such computer: "fedora", "steamdeck")
std::vector<Redaction> private_words();

} // namespace report
