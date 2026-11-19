// The PC renderer's GPU time by kind of work (pcr_gpu_time.h)
#include "pcr_gpu_time.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#ifdef __linux__
#include <pthread.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include "device.hpp"

extern "C" {
#include "port/profile.h"
#include "port/zones.h"
}

using namespace Vulkan;

namespace pcr {

namespace {

const char *const work_names[size_t(GpuWork::count)] = {"2d",         "native 000",   "native 280", "native 700",
                                                        "DATE copy",  "blend copy",   "texture copy", "surface",
                                                        "upload",     "copy",         "picture",    "present",
                                                        "planes"};

// KF4_GSN_GPU_TIME=N (N >= 1): on
bool switched_on() {
    const char *v = getenv("KF4_GSN_GPU_TIME");
    return v && atoi(v) >= 1;
}

// KF4_GPU_LABELS=1: debug labels
bool labels_on() {
    const char *v = getenv("KF4_GPU_LABELS");
    return v && atoi(v) >= 1;
}

// KF4_GPU_DRAWS=FILE (developers', kf4-gsreplay --gpu-draws): every interval written to FILE with the GS draws it was
// for (Canvas::gpu_draw's numbers: kf4-gsreplay --list's), each batch an interval of its own. One file for every
// GpuTimes (lines from two threads interleave whole).
FILE *draws_file() {
    static FILE *file = [] {
        const char *path = getenv("KF4_GPU_DRAWS");
        FILE *f = path && *path ? fopen(path, "w") : nullptr;
        if (f) {
            fprintf(f, "# interval picture work state first_draw last_draw ns\n");
        }
        return f;
    }();
    return file;
}
std::mutex draws_file_lock;

// The profiler's rows: a kind's key, a shader state's (of a kind)
const uint64_t KIND_ROW = uint64_t(1) << 56, SHADER_ROW = uint64_t(2) << 56;

uint32_t field(uint32_t key, int shift, int bits) {
    return (key >> shift) & ((1u << bits) - 1);
}

} // namespace

// program 2 bits at 0, texture 2 at 2, tfx 2 at 4, tcc at 6, fog at 7, blend 8 at 8, blending at 16, shader blend at
// 17, date 2 at 18, depth 2 at 20, depth write at 22, passes 2 at 23; bit 31 always (never 0)
uint32_t shader_state_key(const ShaderState &s) {
    return (s.program & 3u) | (s.texture & 3u) << 2 | (s.tfx & 3u) << 4 | uint32_t(s.alpha_channel) << 6 |
           uint32_t(s.fog) << 7 | uint32_t(s.blending ? s.blend : 0) << 8 | uint32_t(s.blending) << 16 |
           uint32_t(s.shader_blend) << 17 | (s.date & 3u) << 18 | (s.depth & 3u) << 20 |
           uint32_t(s.depth_write) << 22 | (s.passes & 3u) << 23 | 1u << 31;
}

void shader_state_text(uint32_t key, char *text, size_t size) {
    static const char *const programs[4] = {"2d", "native 000", "native 280", "native 700"};
    static const char *const textures[4] = {"untextured", "texels", "surface copy", "sampled"};
    static const char *const functions[4] = {"modulate", "decal", "highlight", "highlight2"};
    static const char *const colours[4] = {"Cs", "Cd", "0", "?"};
    static const char *const alphas[4] = {"As", "Ad", "FIX", "?"};
    static const char *const dates[4] = {"", " | DATE stencil", " | DATE copy", ""};
    static const char *const depths[4] = {"no Z", "Z always", "Z >=", "Z >"};
    char texture[48], blend[48];
    uint32_t b = field(key, 8, 8);

    if (field(key, 2, 2) != 0) {
        snprintf(texture, sizeof(texture), "%s %s%s", textures[field(key, 2, 2)], functions[field(key, 4, 2)],
                 field(key, 6, 1) ? " rgba" : " rgb");
    } else {
        snprintf(texture, sizeof(texture), "untextured");
    }
    if (field(key, 17, 1)) {
        snprintf(blend, sizeof(blend), " | blend in shader (%s-%s)*%s+%s", colours[b & 3], colours[(b >> 2) & 3],
                 alphas[(b >> 4) & 3], colours[(b >> 6) & 3]);
    } else if (field(key, 16, 1)) {
        snprintf(blend, sizeof(blend), " | blend (%s-%s)*%s+%s", colours[b & 3], colours[(b >> 2) & 3],
                 alphas[(b >> 4) & 3], colours[(b >> 6) & 3]);
    } else {
        blend[0] = '\0';
    }
    snprintf(text, size, "%s | %s%s%s%s | %s%s%s", programs[field(key, 0, 2)], texture, field(key, 7, 1) ? " | fog" : "",
             blend, dates[field(key, 18, 2)], depths[field(key, 20, 2)], field(key, 22, 1) ? " write" : "",
             field(key, 23, 2) > 1 ? " | alpha test 2 passes" : "");
}

// The frame profiler's GPU lane (port/include/port/zones.h): the intervals put on the CPU's clock (calibrated
// timestamps, VK_KHR_calibrated_timestamps: CLOCK_MONOTONIC or the performance counter, port_profile_now_ns' clock)
// and written as zones, a kind's run of intervals one zone and each shader state within it one more (detail on).
// Made on the owner's thread at its first interval while the zones record; nothing without calibrated timestamps.
class GpuLane {
public:
    GpuLane(Device &device, double period_ns) : device_(device), period_ns_(period_ns) {}

    // An interval of work (state 0: none) from ticks t0 to t1
    void put(GpuWork work, uint32_t state, uint64_t t0, uint64_t t1) {
        if (!ready()) {
            return;
        }
        uint64_t start = std::max(to_ns(t0), last_end_), end = std::max(to_ns(t1), start);
        // (a gap: the GPU idle between submits, or another thread's work: the kind's zone closed)
        if (kind_open_ && (work != kind_ || start > last_end_ + gap_ns)) {
            close();
        }
        if (!kind_open_) {
            port_zone_lane_begin_at(lane_, kind_id(work), start);
            kind_open_ = true;
            kind_ = work;
        }
        if (state != 0) {
            ZoneId id = state_id(work, state);
            port_zone_lane_begin_at(lane_, id, start);
            port_zone_lane_end_at(lane_, id, end);
        }
        last_end_ = end;
    }

    // The collect's end: the kind open closed (a run of a kind never spans two collects)
    void close() {
        if (kind_open_) {
            port_zone_lane_end_at(lane_, kind_id(kind_), last_end_);
            kind_open_ = false;
        }
    }

private:
    static constexpr uint64_t gap_ns = 2000;
    static constexpr int calibrate_every = 240; // (collects: the clocks drift apart slowly)

    bool ready() {
        if (lane_ == nullptr) {
            if (refused_ || !port_zones_recording()) {
                return false;
            }
            if (!calibrate()) {
                refused_ = true;
                fprintf(stderr, "[pcr] no calibrated timestamps: the frame profiler has no GPU lane\n");
                return false;
            }
            lane_ = port_zone_lane_make(("GPU " + thread_name()).c_str(), 1);
            refused_ = lane_ == nullptr;
            return lane_ != nullptr;
        }
        if (++since_calibrated_ >= calibrate_every) {
            calibrate();
        }
        return true;
    }

    // A pair of the GPU's and the host's clocks read together
    bool calibrate() {
        since_calibrated_ = 0;
        if (!device_.get_device_features().supports_calibrated_timestamps) {
            return false;
        }
        VkCalibratedTimestampInfoKHR infos[2] = {};
        infos[0].sType = infos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
#ifdef _WIN32
        infos[0].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
        infos[0].timeDomain = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
#endif
        infos[1].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
        uint64_t stamps[2] = {}, deviation = 0;
        const VolkDeviceTable &table = device_.get_device_table();
        if (table.vkGetCalibratedTimestampsKHR == nullptr ||
            table.vkGetCalibratedTimestampsKHR(device_.get_device(), 2, infos, stamps, &deviation) != VK_SUCCESS) {
            return false;
        }
#ifdef _WIN32
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        host_ns_ = uint64_t(double(stamps[0]) * 1e9 / double(frequency.QuadPart));
#else
        host_ns_ = stamps[0];
#endif
        device_ticks_ = stamps[1];
        return true;
    }

    uint64_t to_ns(uint64_t ticks) const {
        double ns = double(host_ns_) + double(int64_t(ticks - device_ticks_)) * period_ns_;
        return ns > 0 ? uint64_t(ns) : 0;
    }

    static std::string thread_name() {
        char name[32] = "?";
#ifdef __linux__
        pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
        return name;
    }

    ZoneId kind_id(GpuWork work) {
        ZoneId &id = kind_ids_[size_t(work)];
        if (id == ZONE_NONE) {
            id = port_zone_id((std::string("gpu ") + work_names[size_t(work)]).c_str(), ZONE_GPU);
        }
        return id;
    }

    ZoneId state_id(GpuWork work, uint32_t state) {
        uint64_t key = uint64_t(work) << 32 | state;
        auto found = state_ids_.find(key);
        if (found != state_ids_.end()) {
            return found->second;
        }
        char text[PROFILE_GPU_NAME];
        shader_state_text(state, text, sizeof(text));
        ZoneId id = port_zone_id(text, ZONE_GPU);
        state_ids_.emplace(key, id);
        return id;
    }

    Device &device_;
    double period_ns_;
    ZoneLane *lane_ = nullptr;
    bool refused_ = false;
    int since_calibrated_ = 0;
    uint64_t host_ns_ = 0, device_ticks_ = 0;
    bool kind_open_ = false;
    GpuWork kind_ = GpuWork::draws_2d;
    uint64_t last_end_ = 0;
    std::array<ZoneId, size_t(GpuWork::count)> kind_ids_{};
    std::unordered_map<uint64_t, ZoneId> state_ids_;
};

class GpuTimes::Impl {
public:
    explicit Impl(Device &device)
        : device_(device), on_(switched_on()), labels_(labels_on()), draws_out_(draws_file()),
          period_ns_(double(device.get_gpu_properties().limits.timestampPeriod)), lane_(device, period_ns_) {}

    void begin(CommandBuffer &cmd, GpuWork work, uint32_t state) {
        int detail = port_profile_gpu_detail();
        if (!detail) {
            state = 0;
        }
        // (detail 2, or the draws written: each draw an interval of its own, the same state or not; with the draws
        // written, other work too once the draws it is for change)
        bool each_draw = (detail >= 2 || draws_out_) && state != 0;
        bool same_draws = !draws_out_ || (open_first_ == draw_first_ && open_last_ == draw_last_);
        if (open_ && open_cmd_ == &cmd && open_work_ == work && open_state_ == state && !each_draw && same_draws) {
            return;
        }
        if (open_ && open_cmd_ != &cmd) {
            end(*open_cmd_);
        }
        bool timing = on();
        if (!timing && !labels_) {
            return;
        }
        QueryPoolHandle now;
        if (timing) {
            now = cmd.write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        if (open_ && open_cmd_ == &cmd) {
            close(cmd, now);
        }
        if (labels_) {
            cmd.begin_region(work_names[size_t(work)]);
        }
        open_ = true;
        open_cmd_ = &cmd;
        open_work_ = work;
        open_state_ = state;
        open_start_ = now;
        open_first_ = draw_first_;
        open_last_ = draw_last_;
        open_picture_ = pictures_;
    }

    void set_draws(uint32_t first, uint32_t last) {
        draw_first_ = first;
        draw_last_ = last;
    }

    void end(CommandBuffer &cmd) {
        if (!open_ || open_cmd_ != &cmd) {
            return;
        }
        QueryPoolHandle now;
        if (open_start_) {
            now = cmd.write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        close(cmd, now);
        open_ = false;
        open_cmd_ = nullptr;
    }

    void picture() {
        if (on()) {
            pictures_++;
        }
    }

    void collect() {
        while (!pending_.empty()) {
            const Interval &i = pending_.front();
            if (!i.start->is_signalled() || !i.end->is_signalled()) {
                break;
            }
            uint64_t t0 = i.start->get_timestamp_ticks(), t1 = i.end->get_timestamp_ticks();
            if (t1 > t0) {
                uint64_t ns = uint64_t(double(t1 - t0) * period_ns_);
                ns_[size_t(i.work)] += ns;
                if (port_profile_gpu_recording()) {
                    add_to_profile(i, ns);
                }
                lane_.put(i.work, i.state, t0, t1);
                if (draws_out_) {
                    std::lock_guard<std::mutex> hold(draws_file_lock);
                    fprintf(draws_out_, "%llu %llu %s %08X %u %u %llu\n", (unsigned long long)intervals_written_++,
                            (unsigned long long)i.picture, work_names[size_t(i.work)], i.state, i.first, i.last,
                            (unsigned long long)ns);
                }
            }
            pending_.pop_front();
        }
        lane_.close();
        flush_to_profile();
        if (draws_out_) {
            std::lock_guard<std::mutex> hold(draws_file_lock);
            fflush(draws_out_);
        }
        while (pending_.size() > pending_most) {
            pending_.pop_front();
            dropped_++;
        }
    }

    void take_in(Impl &other) {
        other.device_.wait_idle();
        other.collect();
        for (size_t k = 0; k < ns_.size(); k++) {
            ns_[k] += other.ns_[k];
        }
        pictures_ += other.pictures_;
        dropped_ += other.dropped_ + other.pending_.size();
        other.ns_.fill(0);
        other.pictures_ = other.dropped_ = 0;
        other.pending_.clear();
        on_ = on_ || other.on_;
    }

    void print() {
        if (!on_ && pictures_ == 0) {
            return;
        }
        device_.wait_idle();
        collect();
        double total = 0;
        for (uint64_t ns : ns_) {
            total += double(ns) / 1e6;
        }
        double pictures = double(pictures_ ? pictures_ : 1);
        fprintf(stderr, "[stats] pc renderer GPU time: %.1f ms in all, %.3f ms a picture (%llu pictures):", total,
                total / pictures, (unsigned long long)pictures_);
        for (size_t k = 0; k < ns_.size(); k++) {
            if (ns_[k]) {
                fprintf(stderr, " %s %.3f", work_names[k], double(ns_[k]) / 1e6 / pictures);
            }
        }
        if (!pending_.empty() || dropped_) {
            fprintf(stderr, " (%zu intervals not in, %llu let go)", pending_.size(), (unsigned long long)dropped_);
        }
        fprintf(stderr, "\n");
    }

private:
    struct Interval {
        QueryPoolHandle start, end;
        GpuWork work;
        uint32_t state; // (0: none)
        uint32_t first, last; // (the GS draws it was for: set_draws, KF4_GPU_DRAWS)
        uint64_t picture;     // (the pictures counted before it began)
    };

    // the open interval closed at now (when timed), its label ended
    void close(CommandBuffer &cmd, const QueryPoolHandle &now) {
        if (open_start_ && now) {
            pending_.push_back({open_start_, now, open_work_, open_state_, open_first_, open_last_, open_picture_});
        }
        open_start_.reset();
        if (labels_) {
            cmd.end_region();
        }
    }

    // An interval to the profiler's rows: added up here, and handed over once a collect (each row named once then,
    // not once an interval: thousands a vblank with detail on)
    void add_to_profile(const Interval &i, uint64_t ns) {
        Totals &kind = profile_rows_[KIND_ROW | uint64_t(i.work)];
        kind.ns += ns;
        kind.intervals++;
        if (i.state != 0) {
            Totals &state = profile_rows_[SHADER_ROW | uint64_t(i.work) << 32 | i.state];
            state.ns += ns;
            state.intervals++;
        }
    }

    void flush_to_profile() {
        char name[PROFILE_GPU_NAME];
        for (const auto &row : profile_rows_) {
            uint64_t key = row.first;
            if ((key & SHADER_ROW) != 0) {
                shader_state_text(uint32_t(key), name, sizeof(name));
                port_profile_gpu_add(key, name, PROFILE_GPU_SHADER, row.second.ns, row.second.intervals);
            } else {
                snprintf(name, sizeof(name), "pc %s", work_names[size_t(key & 0xFF)]);
                port_profile_gpu_add(key, name, PROFILE_GPU_KIND, row.second.ns, row.second.intervals);
            }
        }
        profile_rows_.clear();
    }

    struct Totals {
        uint64_t ns = 0, intervals = 0;
    };

    // (at most this many kept waiting for their timestamps: the GPU never far behind, but bounded all the same)
    static constexpr size_t pending_most = size_t(1) << 16;

    bool on() const {
        return on_ || draws_out_ || port_profile_gpu_recording() || port_zones_recording();
    }

    Device &device_;
    bool on_;
    bool labels_;
    FILE *draws_out_; // (KF4_GPU_DRAWS: draws_file)
    uint32_t draw_first_ = 0, draw_last_ = 0, open_first_ = 0, open_last_ = 0;
    uint64_t open_picture_ = 0, intervals_written_ = 0;
    double period_ns_;
    bool open_ = false;
    CommandBuffer *open_cmd_ = nullptr;
    GpuWork open_work_ = GpuWork::draws_2d;
    uint32_t open_state_ = 0;
    QueryPoolHandle open_start_;
    std::deque<Interval> pending_;
    std::unordered_map<uint64_t, Totals> profile_rows_; // (collect's, handed over at its end)
    std::array<uint64_t, size_t(GpuWork::count)> ns_{};
    uint64_t pictures_ = 0, dropped_ = 0;
    GpuLane lane_; // (after period_ns_)
};

GpuTimes::GpuTimes(Device &device) : impl_(std::make_unique<Impl>(device)) {}
GpuTimes::~GpuTimes() = default;

void GpuTimes::begin(CommandBuffer &cmd, GpuWork work) {
    impl_->begin(cmd, work, 0);
}

void GpuTimes::begin_draw(CommandBuffer &cmd, GpuWork work, uint32_t state_key) {
    impl_->begin(cmd, work, state_key);
}

void GpuTimes::set_draws(uint32_t first, uint32_t last) {
    impl_->set_draws(first, last);
}

void GpuTimes::end(CommandBuffer &cmd) {
    impl_->end(cmd);
}

void GpuTimes::picture() {
    impl_->picture();
}

void GpuTimes::collect() {
    impl_->collect();
}

void GpuTimes::take_in(GpuTimes &other) {
    impl_->take_in(*other.impl_);
}

void GpuTimes::print() {
    impl_->print();
}

} // namespace pcr
