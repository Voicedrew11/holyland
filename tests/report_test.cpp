// The problem report's parts (port/report/): names taken out of texts, the issue link and its length, a log's start
// and end, and a zip that reads back as it was written.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "report_bundle.hpp"
#include "report_text.hpp"
#include "zip_archive.hpp"

namespace {

int failed, checked;

void check(const char *what, bool ok) {
    checked++;
    if (!ok)
        failed++;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

void redaction() {
    const std::vector<report::Redaction> words = {
        {"ann", "<user>", true},
        {"/home/ann", "~", false},
        {"ann-pc", "<computer>", true},
    };
    check("the home folder goes before the user's name in it",
          report::redact("disc /home/ann/games/kf4.iso", words) == "disc ~/games/kf4.iso");
    check("a whole word goes", report::redact("user ann logged", words) == "user <user> logged");
    check("a name between dashes goes", report::redact("/tmp/-home-x-ann-y", {{"ann", "<user>", true}}) == "/tmp/-home-x-<user>-y");
    check("a name inside another word stays", report::redact("annual planning", words) == "annual planning");
    check("the computer's name, with its dash, as one word", report::redact("host ann-pc up", words) == "host <computer> up");
    check("case does not matter", report::redact(R"(C:\Users\ANN)", {{R"(c:\users\ann)", "~", false}}) == "~");
    check("an e-mail address goes", report::redact("mail me: a.b+c@example.org.", {}) == "mail me: <e-mail>.");
    check("an @ that is no address stays", report::redact("at @ vblank 5, x@y", {}) == "at @ vblank 5, x@y");
    check("no words, nothing changes", report::redact("plain text", {}) == "plain text");
}

void links() {
    check("a Linux folder's link", report::file_link("/a b/c") == "file:///a%20b/c");
    check("a Windows folder's link", report::file_link("C:/Users/x") == "file:///C:/Users/x");
    check("url_encode keeps the unreserved", report::url_encode("a-Z_0.~") == "a-Z_0.~");
    check("url_encode escapes the rest", report::url_encode("a b\n&=é") == "a%20b%0A%26%3D%C3%A9");
    const std::string url = report::issue_url({"o/r", "", "", "line one\nline two"}, 8000);
    check("the form's link", url.rfind("https://github.com/o/r/issues/new?template=report.yml&details=line%20one%0Aline%20two&body=", 0) == 0);
    check("a plain issue's text too, for when the form is missing",
          url.find("&body=Please%20drag") != std::string::npos && url.find("%60%60%60%0Aline%20one%0Aline%20two") != std::string::npos);
    std::string long_details;
    for (int k = 0; k < 2000; k++)
        long_details += "detail line " + std::to_string(k) + "\n";
    const std::string cut = report::issue_url({"o/r", "Title", "what", long_details}, 4000);
    check("a long link is cut to fit", cut.size() <= 4000);
    check("the cut keeps the title and what happened",
          cut.find("&title=Title") != std::string::npos && cut.find("&what=what") != std::string::npos);
    check("the cut says so", cut.find(report::url_encode("(cut short")) != std::string::npos);
    const std::string tiny = report::issue_url({"o/r", "", std::string(3000, 'w'), long_details}, 300);
    check("what happened is cut last, and the link still fits", tiny.size() <= 300);
}

void logs() {
    const std::string text = "1\n2\n3\n4\n5\n6\n7\n8\n9\n";
    check("a short text stays whole", report::head_and_tail(text, 3, 100) == text);
    const std::string kept = report::head_and_tail(text, 2, 5);
    check("the head's lines and the tail from a line's start",
          kept.rfind("1\n2\n", 0) == 0 && kept.size() >= 4 && kept.substr(kept.size() - 4) == "8\n9\n");
    check("what was left out is said", kept.find("bytes left out") != std::string::npos);
}

void zips() {
    std::vector<report::ZipEntry> entries = {
        {"README.txt", {'h', 'i', '\n'}},
        {"logs/game.log", {}},
        {"empty.bin", {}},
    };
    for (int k = 0; k < 100000; k++)
        entries[1].data.push_back(std::uint8_t("[gs] a line of the log\n"[k % 23]));
    for (int k = 0; k < 5000; k++)
        entries.push_back({}), entries.back().name = "many/" + std::to_string(k), entries.back().data = {std::uint8_t(k)};
    const auto archive = report::zip_archive(entries, 1795000000);
    check("the archive is made", archive.has_value());
    if (!archive)
        return;
    const auto log_only = report::zip_archive({entries[1]}, 1795000000);
    check("the log is deflated", log_only && log_only->size() < entries[1].data.size() / 10);
    const auto back = report::zip_entries(*archive);
    check("it reads back", back.has_value() && back->size() == entries.size());
    bool same = back.has_value();
    for (size_t k = 0; same && k < entries.size(); k++)
        same = (*back)[k].name == entries[k].name && (*back)[k].data == entries[k].data;
    check("every entry as it was", same);
    std::vector<std::uint8_t> broken = *archive;
    broken[40] ^= 0xFF; // (in the first entry's data or header)
    check("a broken archive is refused", !report::zip_entries(broken).has_value());
    // (a copy for checking with other tools: report_test FILE)
}

// A bundle: texts redacted, saves within their budget, written as a zip that reads back
void bundles() {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / "holyland-report-test";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder / "saves");
    std::ofstream(folder / "saves" / "a.kf4save", std::ios::binary) << "save a";
    std::ofstream(folder / "saves" / "big.kf4save", std::ios::binary) << std::string(9u << 20, 'x');
    std::ofstream(folder / "log.txt", std::ios::binary) << "line from /home/ann\n";
    report::ReportBundle bundle({{"/home/ann", "~", false}});
    bundle.add_text("t.txt", "at /home/ann/x");
    check("a log is read", bundle.add_log("logs/log.txt", folder / "log.txt", 10, 100));
    check("a missing log says so", !bundle.add_log("logs/none.txt", folder / "none.txt", 10, 100));
    bundle.add_save_folder("saves/", folder / "saves");
    const auto zip = bundle.write(folder / "out", "r");
    check("the bundle is written", zip.has_value() && fs::exists(*zip) && !fs::exists(folder / "out" / "r.zip.tmp"));
    if (!zip)
        return;
    std::ifstream in(*zip, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto entries = report::zip_entries(bytes);
    bool text = false, log = false, save = false, big = false;
    for (const report::ZipEntry &e : entries.value_or(std::vector<report::ZipEntry>{})) {
        const std::string body(e.data.begin(), e.data.end());
        text = text || (e.name == "t.txt" && body == "at ~/x");
        log = log || (e.name == "logs/log.txt" && body == "line from ~\n");
        save = save || (e.name == "saves/a.kf4save" && body == "save a");
        big = big || e.name == "saves/big.kf4save";
    }
    check("texts and logs redacted", text && log);
    check("a save kept, one past 8 MB left out", save && !big);
    fs::remove_all(folder, error);
}

} // namespace

int main(int argc, char **argv) {
    redaction();
    links();
    logs();
    zips();
    bundles();
    if (argc > 1) {
        std::vector<report::ZipEntry> sample = {{"a.txt", {'a', '\n'}}, {"b/c.txt", std::vector<std::uint8_t>(5000, 'c')}};
        const auto archive = report::zip_archive(sample, 1795000000);
        std::FILE *out = std::fopen(argv[1], "wb");
        if (archive && out != nullptr)
            std::fwrite(archive->data(), 1, archive->size(), out);
        if (out != nullptr)
            std::fclose(out);
    }
    std::printf("%d of %d checks failed\n", failed, checked);
    return failed == 0 ? 0 : 1;
}
