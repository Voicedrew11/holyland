// The problem report's texts (port/report/): what keeps the player's own names out of what is posted, the issue
// link, and the parts of a long log kept. No files, no system: tested in tests/report_test.cpp. C++17: the launcher
// builds it too.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace report {

// A private word and what it becomes ("/home/ann" -> "~", "ann" -> "<user>")
struct Redaction {
    std::string word;
    std::string stand_in;
    bool whole_word; // only where no letter, digit or '_' touches it (a short user name inside other words stays)
};

// The text with each word replaced (the longest first, so a home folder goes before the user's name in it) and
// e-mail addresses as <e-mail>. Words compare without case: Windows' folders are written either way.
std::string redact(std::string_view text, const std::vector<Redaction> &words);

// A value for a link's query: everything but letters, digits and -_.~ as %XX (UTF-8 bytes)
std::string url_encode(std::string_view text);

// The repository's issue form (.github/ISSUE_TEMPLATE/report.yml), its title and boxes filled in
struct IssueForm {
    std::string_view repository; // "owner/name"
    std::string_view title;
    std::string_view what;    // the form's "What happened"
    std::string_view details; // the form's "Details"
};
// The form's link, at most max_bytes long: Details cut first (at a line's end, with a note), then What happened. The
// same text also goes as a plain issue's body, which GitHub shows when the form is not on the default branch.
std::string issue_url(const IssueForm &form, std::size_t max_bytes);

// A local folder or file as a file:// link, each byte that needs it escaped ("/a b" -> "file:///a%20b";
// Windows' "C:/x" -> "file:///C:/x")
std::string file_link(std::string_view generic_path);

// The first head_lines lines and the last tail_bytes bytes (from a line's start) of a text, with a line saying how
// much was left out between; the text as it is when it is that short
std::string head_and_tail(std::string_view text, std::size_t head_lines, std::size_t tail_bytes);

} // namespace report
