// The frame profiler's owner (port/include/port/frame_profiler.h)
#include "port/frame_profiler.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/zone_record.hpp"
#include "port/thread_name.h"
#include "port/zone_scope.hpp"

extern "C" {
#include "port/profile.h"
#include "port/zones.h"
}

namespace {

const auto TAKE_EVERY = std::chrono::milliseconds(50); // (the pump writes millions of events a second)
const double REPORT_SECONDS = 5.0;

// What the environment asked for (start's; read by the thread)
struct Asked {
    bool console = false;
    std::string csv_path;
    double spike_ms = 0.0;
};

class Owner {
public:
    void start(const Asked &asked) {
        asked_ = asked;
        if (!asked_.csv_path.empty()) {
            csv_ = std::fopen(asked_.csv_path.c_str(), "w");
            if (csv_ == nullptr) {
                std::fprintf(stderr, "[profile] can't write %s\n", asked_.csv_path.c_str());
            } else {
                std::fprintf(csv_, "frame,start_ms,lane,zone,kind,self_ms,incl_ms,calls\n");
                std::fprintf(stderr, "[profile] every frame to %s\n", asked_.csv_path.c_str());
            }
        }
        if (asked_.console || csv_ != nullptr || asked_.spike_ms > 0) {
            want(1);
        }
    }

    void want(int more) {
        std::lock_guard<std::mutex> hold(control_);
        wanted_ += more;
        port_zones_want(more);
        if (wanted_ > 0 && !thread_.joinable()) {
            stopping_ = false;
            thread_ = std::thread([this] { run(); });
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> hold(control_);
            stopping_ = true;
            wake_.notify_all();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (csv_ != nullptr) {
            std::fclose(csv_);
            csv_ = nullptr;
        }
    }

    bool with_record(const std::function<void(zones::Record &)> &f) {
        std::unique_lock<std::mutex> hold(record_lock_, std::try_to_lock);
        if (!hold.owns_lock()) {
            return false;
        }
        f(record_);
        return true;
    }

private:
    // ("kf4-profiler") Four times a second while wanted: the rings read, done frames written and reported
    void run() {
        port_name_thread("kf4-profiler");
        std::unique_lock<std::mutex> hold(control_);
        while (!stopping_) {
            wake_.wait_for(hold, TAKE_EVERY, [&] { return stopping_; });
            if (wanted_ <= 0 && !stopping_) {
                continue; // (kept: the panel may open again; the record holds what it had)
            }
            hold.unlock();
            take();
            hold.lock();
        }
    }

    void take() {
        static const ZoneId zone = port_zone_id("profiler: the rings read into frames", ZONE_WORK);
        ZoneScope scope(zone); // (its own cost, on its own lane)
        std::lock_guard<std::mutex> hold(record_lock_);
        uint64_t lost_before = record_.lost(), t0 = port_profile_now_ns();
        record_.take();
        uint64_t took = port_profile_now_ns() - t0;
        if (record_.lost() != lost_before) {
            std::string lanes;
            for (int i = 0; i < record_.lanes(); i++) {
                if (record_.lane_lost(i)) {
                    lanes += " " + record_.lane_name(i) + "=" + std::to_string(record_.lane_lost(i));
                }
            }
            std::fprintf(stderr, "[profile] events lost (the take took %.1f ms):%s\n", double(took) / 1e6, lanes.c_str());
        }
        const std::deque<zones::Frame> &frames = record_.frames();
        for (const zones::Frame &f : frames) {
            if (f.index < next_out_) {
                continue;
            }
            next_out_ = f.index + 1;
            if (csv_ != nullptr) {
                write_csv(f);
            }
            if (asked_.spike_ms > 0 && f.ms() > asked_.spike_ms) {
                std::fprintf(stderr, "[profile] spike: frame %llu, %.2f ms: %s; longest waits: %s\n",
                             (unsigned long long)f.index, f.ms(), largest(f, 6, ZONE_WORK).c_str(),
                             largest(f, 3, ZONE_WAIT).c_str());
            }
            if (asked_.console) {
                add_to_report(f);
            }
        }
    }

    void write_csv(const zones::Frame &f) {
        double start_ms = double(f.start_ns) / 1e6;
        std::fprintf(csv_, "%llu,%.3f,,frame,,%.4f,%.4f,\n", (unsigned long long)f.index, start_ms, f.ms(), f.ms());
        static const char *const kinds[ZONE_KINDS] = {"work", "wait", "gpu"};
        for (const zones::ZoneTime &z : f.zones) {
            std::string name = port_zone_name(z.id);
            std::replace(name.begin(), name.end(), ',', ';');
            std::fprintf(csv_, "%llu,%.3f,%s,%s,%s,%.4f,%.4f,%u\n", (unsigned long long)f.index, start_ms,
                         record_.lane_name(z.lane).c_str(), name.c_str(), kinds[port_zone_kind(z.id)],
                         double(z.self_ns) / 1e6, double(z.incl_ns) / 1e6, z.calls);
        }
    }

    // The frame's n largest self times: of waits (ZONE_WAIT), or of everything else (work and GPU)
    std::string largest(const zones::Frame &f, size_t n, int kind) const {
        std::vector<const zones::ZoneTime *> order;
        for (const zones::ZoneTime &z : f.zones) {
            bool is_wait = port_zone_kind(z.id) == ZONE_WAIT;
            if (is_wait == (kind == ZONE_WAIT) && z.self_ns > 0) {
                order.push_back(&z);
            }
        }
        std::sort(order.begin(), order.end(),
                  [](const zones::ZoneTime *a, const zones::ZoneTime *b) { return a->self_ns > b->self_ns; });
        std::string text;
        char part[160];
        for (size_t i = 0; i < order.size() && i < n; i++) {
            std::snprintf(part, sizeof(part), "%s%s/%s %.2f", i ? ", " : "", record_.lane_name(order[i]->lane).c_str(),
                          port_zone_name(order[i]->id), double(order[i]->self_ns) / 1e6);
            text += part;
        }
        return text;
    }

    // The console's summary: every REPORT_SECONDS, frames' lengths and each lane's busiest zones a frame
    void add_to_report(const zones::Frame &f) {
        if (report_frames_.empty()) {
            report_start_ns_ = f.start_ns;
        }
        report_frames_.push_back(f.ms());
        for (const zones::ZoneTime &z : f.zones) {
            uint32_t key = uint32_t(z.lane) << 16 | z.id;
            report_self_[key] += z.self_ns;
        }
        if (double(f.end_ns - report_start_ns_) / 1e9 < REPORT_SECONDS) {
            return;
        }
        size_t n = report_frames_.size();
        double sum = 0;
        for (double ms : report_frames_) {
            sum += ms;
        }
        std::sort(report_frames_.begin(), report_frames_.end());
        double p99 = report_frames_[std::min(n - 1, size_t(double(n) * 0.99))];
        std::vector<std::pair<uint32_t, uint64_t>> order(report_self_.begin(), report_self_.end());
        std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::string top;
        char part[160];
        int shown = 0;
        for (const auto &o : order) {
            ZoneId id = ZoneId(o.first & 0xFFFF);
            if (port_zone_kind(id) == ZONE_WAIT) {
                continue;
            }
            std::snprintf(part, sizeof(part), "%s%s/%s %.3f", shown ? ", " : "",
                          record_.lane_name(int(o.first >> 16)).c_str(), port_zone_name(id),
                          double(o.second) / 1e6 / double(n));
            top += part;
            if (++shown == 12) {
                break;
            }
        }
        std::fprintf(stderr, "[profile] %.1f pictures/s, %.2f ms avg, %.2f p99, %.2f max; ms a picture: %s%s\n",
                     double(n) / (double(f.end_ns - report_start_ns_) / 1e9), sum / double(n), p99,
                     report_frames_.back(), top.c_str(),
                     record_.lost() ? " (events lost: the ring outran the reader)" : "");
        report_frames_.clear();
        report_self_.clear();
    }

    Asked asked_;
    std::FILE *csv_ = nullptr;
    std::mutex control_; // (wanted_, stopping_, thread_)
    std::condition_variable wake_;
    int wanted_ = 0;
    bool stopping_ = false;
    std::thread thread_;
    std::mutex record_lock_; // (record_ and what take writes)
    zones::Record record_;
    uint64_t next_out_ = 0;
    std::vector<double> report_frames_;
    std::map<uint32_t, uint64_t> report_self_;
    uint64_t report_start_ns_ = 0;
};

Owner &owner() {
    static Owner *o = new Owner(); // (never destroyed: threads may still call in at exit)
    return *o;
}

} // namespace

extern "C" void port_frame_profiler_start(void) {
    Asked asked;
    const char *on = std::getenv("KF4_PROFILE");
    asked.console = on != nullptr && std::atoi(on) >= 1;
    if (const char *out = std::getenv("KF4_PROFILE_OUT")) {
        asked.csv_path = out;
    }
    if (const char *spike = std::getenv("KF4_PROFILE_SPIKE")) {
        asked.spike_ms = std::atof(spike);
    }
    if (const char *detail = std::getenv("KF4_GPU_DETAIL")) {
        port_profile_gpu_set_detail(std::atoi(detail)); // (1: by shader state, 2: each draw)
    }
    owner().start(asked);
}

extern "C" void port_frame_profiler_stop(void) {
    owner().stop();
}

extern "C" void port_frame_profiler_want(int more) {
    owner().want(more);
}

namespace zones {
bool with_record(const std::function<void(Record &)> &f) {
    return owner().with_record(f);
}
} // namespace zones
