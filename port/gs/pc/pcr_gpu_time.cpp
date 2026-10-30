// The PC renderer's GPU time by kind of work (pcr_gpu_time.h)
#include "pcr_gpu_time.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>

#include "device.hpp"

extern "C" {
#include "port/profile.h"
}

using namespace Vulkan;

namespace pcr {

namespace {

const char *const work_names[size_t(GpuWork::count)] = {"2d",           "native 000", "native 280", "native 700",
                                                        "DATE copy",    "texture copy", "surface",  "upload",
                                                        "copy",         "picture"};

// KF4_GSN_GPU_TIME=N (N >= 1): on
bool switched_on() {
    const char *v = getenv("KF4_GSN_GPU_TIME");
    return v && atoi(v) >= 1;
}

} // namespace

class GpuTimes::Impl {
public:
    explicit Impl(Device &device)
        : device_(device), on_(switched_on()),
          period_ns_(double(device.get_gpu_properties().limits.timestampPeriod)) {}

    void begin(CommandBuffer &cmd, GpuWork work) {
        if (open_ && open_cmd_ == &cmd && open_work_ == work) {
            return;
        }
        if (open_ && open_cmd_ != &cmd) {
            end(*open_cmd_);
        }
        if (!on()) {
            return;
        }
        QueryPoolHandle now = cmd.write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        if (open_ && open_cmd_ == &cmd) {
            pending_.push_back({open_start_, now, open_work_});
        }
        open_ = true;
        open_cmd_ = &cmd;
        open_work_ = work;
        open_start_ = now;
    }

    void end(CommandBuffer &cmd) {
        if (!open_ || open_cmd_ != &cmd) {
            return;
        }
        QueryPoolHandle now = cmd.write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        pending_.push_back({open_start_, now, open_work_});
        open_ = false;
        open_cmd_ = nullptr;
        open_start_.reset();
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
            }
            pending_.pop_front();
        }
        while (pending_.size() > pending_most) {
            pending_.pop_front();
            dropped_++;
        }
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
    };

    // (at most this many kept waiting for their timestamps: the GPU never far behind, but bounded all the same)
    static constexpr size_t pending_most = size_t(1) << 16;

    bool on() const {
        return on_ || port_profile_on();
    }

    Device &device_;
    bool on_;
    double period_ns_;
    bool open_ = false;
    CommandBuffer *open_cmd_ = nullptr;
    GpuWork open_work_ = GpuWork::draws_2d;
    QueryPoolHandle open_start_;
    std::deque<Interval> pending_;
    std::array<uint64_t, size_t(GpuWork::count)> ns_{};
    uint64_t pictures_ = 0, dropped_ = 0;
};

GpuTimes::GpuTimes(Device &device) : impl_(std::make_unique<Impl>(device)) {}
GpuTimes::~GpuTimes() = default;

void GpuTimes::begin(CommandBuffer &cmd, GpuWork work) {
    impl_->begin(cmd, work);
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

void GpuTimes::print() {
    impl_->print();
}

} // namespace pcr
