// A report's files on their way into its zip (port/report/): texts redacted as they come in, logs cut to their start
// and end, saves kept within a budget, then written as one zip. The game's Report an Issue (problem_report.cpp) and
// the launcher's crash report (launcher/crash_report.cpp) each fill one. C++17.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "report_text.hpp"
#include "zip_archive.hpp"

namespace report {

class ReportBundle {
public:
    explicit ReportBundle(std::vector<Redaction> words) : words_(std::move(words)) {
    }

    // A text, redacted
    void add_text(const std::string &name, std::string_view text);
    // A file as it is (a picture); false when it could not be read
    bool add_file(const std::string &name, const std::filesystem::path &path);
    // A text file's first head_lines lines and its last tail_bytes, redacted; false when it could not be read
    bool add_log(const std::string &name, const std::filesystem::path &path, std::size_t head_lines,
                 std::size_t tail_bytes);
    // A save as it is: nothing past 8 MB a file or 32 MB of saves in all
    void add_save(const std::string &name, const std::filesystem::path &path);
    // Each file of a folder as a save, as prefix + its name
    void add_save_folder(const std::string &prefix, const std::filesystem::path &folder);

    // The zip, written whole as <folder>/<stem>.zip (a .tmp put in place, the folder made): its path, or nullopt
    std::optional<std::filesystem::path> write(const std::filesystem::path &folder, const std::string &stem) const;

    const std::vector<Redaction> &words() const {
        return words_;
    }
    std::string redacted(std::string_view text) const {
        return redact(text, words_);
    }

private:
    std::vector<Redaction> words_;
    std::vector<ZipEntry> entries_;
    std::uintmax_t save_bytes_ = 0;
};

// "20261124-153012", local time: a report's name
std::string report_timestamp();
// A file whole, or nullopt
std::optional<std::string> read_text(const std::filesystem::path &path);
// The file was written less than `age` ago
bool written_within(const std::filesystem::path &path, std::chrono::hours age);
// KF4_REPORT_NO_OPEN=1 (tests): nothing is opened; the link goes to <zip>.url.txt instead. True when it did that.
bool keep_link_instead(const std::filesystem::path &zip, const std::string &url);

} // namespace report
