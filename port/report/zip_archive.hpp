// A zip archive made in memory (port/report/): the problem report's one file to attach. Each entry deflated (miniz's
// deflate, from libchdr's copy), or stored when that is no smaller. No ZIP64: an entry or the archive past 4 GB is
// refused, which a report (tens of MB at most) never comes near. C++17: the launcher builds it too.
#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace report {

struct ZipEntry {
    std::string name; // its path in the archive, '/' between folders
    std::vector<std::uint8_t> data;
};

// The archive's bytes, every entry dated `made` (local time); nullopt past the format's 4 GB or 65535 entries
std::optional<std::vector<std::uint8_t>> zip_archive(const std::vector<ZipEntry> &entries, std::time_t made);

// An archive's entries read back (the tests'): nullopt when it is not one zip_archive could have made
std::optional<std::vector<ZipEntry>> zip_entries(const std::vector<std::uint8_t> &archive);

} // namespace report
