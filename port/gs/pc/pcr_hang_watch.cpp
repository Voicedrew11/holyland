// (pcr_hang_watch.h)
#include "pcr_hang_watch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

#include "port/thread_name.h"

namespace pcr {

double hang_clock_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string hang_report(const HangNotes &notes, const char *where, double waited_s, double now_s) {
    std::string out;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "The GPU has not answered for %.0f s (waiting in %s). The renderer's last submits, newest last:\n",
                  waited_s, where ? where : "?");
    out += line;
    for (const SubmitNote &n : notes.notes()) {
        std::snprintf(line, sizeof(line), "  #%llu  %8.3f s before  %-12s %s\n", (unsigned long long)n.serial,
                      now_s - n.at_s, n.who, n.what);
        out += line;
    }
    out += "The kernel's side: journalctl -k | grep -A3 'ring gfx'; within five minutes, as root,\n"
           "cat /sys/class/devcoredump/devcd*/data (Linux, amdgpu).\n";
    return out;
}

class HangWatch::Impl {
public:
    Impl(const HangNotes &notes, std::string folder, int hang_seconds)
        : notes_(notes), folder_(std::move(folder)), hang_seconds_(hang_seconds) {
        thread_ = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard<std::mutex> hold(lock_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

private:
    // (the watch's thread, "kf4-pc-watch")
    void run() {
        port_name_thread("kf4-pc-watch");
        const char *watched = nullptr;
        int looks = 0;
        std::unique_lock<std::mutex> hold(lock_);
        while (!wake_.wait_for(hold, std::chrono::seconds(1), [this] { return stop_; })) {
            const char *where = nullptr;
            double now = hang_clock_s();
            double waited = notes_.longest_wait(now, &where);
            // (the same wait seen a second older at each of hang_seconds_ looks in a row)
            looks = where != nullptr && where == watched && waited >= looks ? looks + 1 : 0;
            watched = where;
            if (looks >= hang_seconds_) {
                report(where, waited, now);
                std::_Exit(HANG_EXIT_CODE);
            }
        }
    }

    void report(const char *where, double waited_s, double now_s) const {
        std::string text = hang_report(notes_, where, waited_s, now_s);
        std::fprintf(stderr, "[pcr] %s", text.c_str());
        char name[64];
        std::time_t t = std::time(nullptr);
        std::tm tm = {};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(name, sizeof(name), "gpu_hang_%Y%m%d_%H%M%S.txt", &tm);
        std::string path = folder_.empty() ? std::string(name) : folder_ + "/" + name;
        if (std::FILE *f = std::fopen(path.c_str(), "w")) {
            if (std::fputs(text.c_str(), f) < 0 || std::fclose(f) != 0) {
                std::fprintf(stderr, "[pcr] the hang's report could not be written to %s\n", path.c_str());
            } else {
                std::fprintf(stderr, "[pcr] the hang's report: %s\n", path.c_str());
            }
        } else {
            std::fprintf(stderr, "[pcr] the hang's report could not be written to %s\n", path.c_str());
        }
        std::fflush(stderr);
    }

    const HangNotes &notes_;
    const std::string folder_;
    const int hang_seconds_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

HangWatch::HangWatch(const HangNotes &notes, std::string folder, int hang_seconds)
    : impl_(std::make_unique<Impl>(notes, std::move(folder), hang_seconds)) {}
HangWatch::~HangWatch() = default;

} // namespace pcr
