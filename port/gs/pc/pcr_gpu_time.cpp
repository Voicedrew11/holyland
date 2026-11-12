// The PC renderer's GPU time by kind of work (pcr_gpu_time.h)
#include "pcr_gpu_time.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_map>

#include "device.hpp"

extern "C" {
#include "port/profile.h"
}

using namespace Vulkan;

namespace pcr {

namespace {

const char *const work_names[size_t(GpuWork::count)] = {"2d",         "native 000",   "native 280", "native 700",
                                                        "DATE copy",  "blend copy",   "texture copy", "surface",
                                                        "upload",     "copy",         "picture",    "present"};

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

class GpuTimes::Impl {
public:
    explicit Impl(Device &device)
        : device_(device), on_(switched_on()), labels_(labels_on()),
          period_ns_(double(device.get_gpu_properties().limits.timestampPeriod)) {}

    void begin(CommandBuffer &cmd, GpuWork work, uint32_t state) {
        if (!port_profile_gpu_detail()) {
            state = 0;
        }
        if (open_ && open_cmd_ == &cmd && open_work_ == work && open_state_ == state) {
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
                if (port_profile_on()) {
                    port_profile_add(PROFILE_GPU, ns);
                }
                if (port_profile_gpu_recording()) {
                    add_to_profile(i, ns);
                }
            }
            pending_.pop_front();
        }
        flush_to_profile();
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
    };

    // the open interval closed at now (when timed), its label ended
    void close(CommandBuffer &cmd, const QueryPoolHandle &now) {
        if (open_start_ && now) {
            pending_.push_back({open_start_, now, open_work_, open_state_});
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
        return on_ || port_profile_on() || port_profile_gpu_recording();
    }

    Device &device_;
    bool on_;
    bool labels_;
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
};

GpuTimes::GpuTimes(Device &device) : impl_(std::make_unique<Impl>(device)) {}
GpuTimes::~GpuTimes() = default;

void GpuTimes::begin(CommandBuffer &cmd, GpuWork work) {
    impl_->begin(cmd, work, 0);
}

void GpuTimes::begin_draw(CommandBuffer &cmd, GpuWork work, uint32_t state_key) {
    impl_->begin(cmd, work, state_key);
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
