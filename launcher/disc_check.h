// Is this the player's King's Field IV disc (docs/port/LAUNCHER.md, "The disc")? The same files the game
// requires (port/platform/disc.c: SYSTEM.CNF and DATA\KF4.DAT), read from an .iso with the game's own
// ISO 9660 reader (port/platform/iso9660.c) or from an extracted disc's folder, plus the release
// SYSTEM.CNF boots: the port is built from the USA release.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace launcher {

enum class DiscVerdict {
    ok,
    missing,     // no such file or folder
    unreadable,  // not an ISO 9660 image, or a broken one
    not_kf4,     // no SYSTEM.CNF or DATA\KF4.DAT
    other_release, // King's Field IV, but not the release the port is built from
};

struct DiscCheck {
    DiscVerdict verdict = DiscVerdict::missing;
    std::string message; // for the player; "" when ok
    std::string boot;    // what SYSTEM.CNF boots ("SLUS_203.18"), when read
};

// An .iso, or a folder (or a file in it, such as its SYSTEM.CNF: the folder is meant)
DiscCheck check_disc(const std::filesystem::path &path);

// The verdict from what was found on the disc: SYSTEM.CNF's text ("" when missing) and whether
// DATA\KF4.DAT is there. Separate from the reading, for the tests.
DiscCheck judge_disc(std::string_view system_cnf, bool has_kf4_dat);

// the folder check_disc means by `path` (a file in an extracted disc means its folder)
std::filesystem::path disc_meant(const std::filesystem::path &path);

} // namespace launcher
