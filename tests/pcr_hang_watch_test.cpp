// The watch for a GPU that has stopped answering (port/gs/pc/pcr_hang_notes.h, pcr_hang_watch.h): the last submits
// kept oldest first, the longest wait found, the report's text; and with `hang <this program> <folder>`, a wait that
// never ends ending a process with HANG_EXIT_CODE and the report written, while one that ends in time does not.
#include "pcr_hang_watch.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

void notes_kept_oldest_first() {
    HangNotes notes;
    CHECK(notes.notes().empty());
    notes.submitted("kf4-pc", "one", 1.0);
    notes.submitted("kf4-pc-pump", "two", 2.0);
    std::vector<SubmitNote> two = notes.notes();
    CHECK(two.size() == 2);
    CHECK(two[0].serial == 1 && std::string(two[0].what) == "one" && std::string(two[0].who) == "kf4-pc");
    CHECK(two[1].serial == 2 && two[1].at_s == 2.0);

    // (more than are kept: the newest HangNotes::kept, oldest first)
    for (int i = 3; i <= int(HangNotes::kept) + 10; i++) {
        notes.submitted("kf4-pc", std::to_string(i).c_str(), double(i));
    }
    std::vector<SubmitNote> all = notes.notes();
    CHECK(all.size() == HangNotes::kept);
    CHECK(all.front().serial == 11 && std::string(all.front().what) == "11");
    CHECK(all.back().serial == HangNotes::kept + 10);
    for (size_t i = 1; i < all.size(); i++) {
        CHECK(all[i].serial == all[i - 1].serial + 1);
    }
}

void long_names_cut_not_overrun() {
    HangNotes notes;
    std::string who(100, 'w'), what(1000, 'x');
    notes.submitted(who.c_str(), what.c_str(), 0);
    SubmitNote n = notes.notes().at(0);
    CHECK(std::string(n.who).size() == sizeof(n.who) - 1);
    CHECK(std::string(n.what).size() == sizeof(n.what) - 1);
}

void longest_wait_found() {
    HangNotes notes;
    const char *where = "set";
    CHECK(notes.longest_wait(10.0, &where) == 0 && where == nullptr);
    int a = notes.wait_begins("a", 4.0);
    int b = notes.wait_begins("b", 1.0);
    CHECK(a >= 0 && b >= 0 && a != b);
    CHECK(notes.longest_wait(10.0, &where) == 9.0 && std::string(where) == "b");
    notes.wait_ends(b);
    CHECK(notes.longest_wait(10.0, &where) == 6.0 && std::string(where) == "a");
    notes.wait_ends(a);
    CHECK(notes.longest_wait(10.0, &where) == 0 && where == nullptr);

    // (more waits than slots: not watched, and ending one is nothing)
    int slots[HangNotes::waits_most];
    for (int &slot : slots) {
        slot = notes.wait_begins("many", 0);
        CHECK(slot >= 0);
    }
    CHECK(notes.wait_begins("one more", 0) == -1);
    notes.wait_ends(-1);
    notes.wait_ends(HangNotes::waits_most);
    for (int slot : slots) {
        notes.wait_ends(slot);
    }
    CHECK(notes.longest_wait(10.0, &where) == 0);

    {
        GpuWait wait(&notes, "scoped", 2.0);
        CHECK(notes.longest_wait(5.0, &where) == 3.0 && std::string(where) == "scoped");
        GpuWait unwatched(nullptr, "none", 0);
    }
    CHECK(notes.longest_wait(5.0, &where) == 0);
}

void report_names_the_wait_and_the_notes() {
    HangNotes notes;
    notes.submitted("kf4-pc", "canvas, vblank 7", 98.5);
    notes.submitted("kf4-pc-pump", "a picture", 99.75);
    std::string text = hang_report(notes, "kf4-pc's next_frame_context", 15.2, 100.0);
    CHECK(text.find("15 s") != std::string::npos);
    CHECK(text.find("kf4-pc's next_frame_context") != std::string::npos);
    size_t first = text.find("canvas, vblank 7"), second = text.find("a picture");
    CHECK(first != std::string::npos && second != std::string::npos && first < second);
    CHECK(text.find("1.500 s before") != std::string::npos);
    CHECK(text.find("0.250 s before") != std::string::npos);
    CHECK(hang_report(notes, nullptr, 1, 100.0).find("waiting in ?") != std::string::npos);
}

// (the child) A wait of `wait_s` seconds under a watch of 2: ends by the watch (HANG_EXIT_CODE) or returns 0
int child(const char *folder, double wait_s) {
    HangNotes notes;
    notes.submitted("test", "the stuck job", hang_clock_s());
    HangWatch watch(notes, folder, 2);
    {
        GpuWait wait(&notes, "the test's wait", hang_clock_s());
        std::this_thread::sleep_for(std::chrono::duration<double>(wait_s));
    }
    // (waits that each end in time, one after another at the same place, are not a hang)
    for (int i = 0; i < 8; i++) {
        GpuWait wait(&notes, "the test's wait", hang_clock_s());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}

int exit_code_of(const std::string &command) {
#ifdef _WIN32
    // (cmd /c drops the first and the last quote of a line that starts with one and has more: the whole line quoted
    // once more keeps the program's path and the folder's quoted)
    int status = std::system(("\"" + command + "\"").c_str());
    return status;
#else
    int status = std::system(command.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

size_t reports_in(const std::filesystem::path &folder, std::string *text) {
    size_t found = 0;
    for (const auto &entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().filename().string().rfind("gpu_hang_", 0) != 0) {
            continue;
        }
        found++;
        if (std::FILE *f = std::fopen(entry.path().string().c_str(), "rb")) {
            char buffer[4096];
            size_t got = std::fread(buffer, 1, sizeof(buffer), f);
            text->assign(buffer, got);
            std::fclose(f);
        }
    }
    return found;
}

void a_hang_ends_the_process(const std::string &program, const std::filesystem::path &folder) {
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    std::filesystem::create_directories(folder, error);
    CHECK(!error);
    std::string quoted = "\"" + program + "\" child \"" + folder.string() + "\" ";
    std::string text;

    CHECK(exit_code_of(quoted + "0.2") == 0);
    CHECK(reports_in(folder, &text) == 0);

    CHECK(exit_code_of(quoted + "60") == HANG_EXIT_CODE);
    CHECK(reports_in(folder, &text) == 1);
    CHECK(text.find("the test's wait") != std::string::npos);
    CHECK(text.find("the stuck job") != std::string::npos);
    std::filesystem::remove_all(folder, error);
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "child") {
        return child(argv[2], std::atof(argv[3]));
    }
    if (argc == 4 && std::string(argv[1]) == "hang") {
        a_hang_ends_the_process(argv[2], argv[3]);
    } else {
        notes_kept_oldest_first();
        long_names_cut_not_overrun();
        longest_wait_found();
        report_names_the_wait_and_the_notes();
    }
    if (failures == 0) {
        std::printf("ok\n");
    }
    return failures == 0 ? 0 : 1;
}
