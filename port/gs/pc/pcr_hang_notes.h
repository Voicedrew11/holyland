// What the PC renderer last gave the GPU, kept for the day the GPU stops answering (pcr_hang_watch.h).
//
// A GPU hang (amdgpu's "ring gfx timeout", then a reset of the whole card) leaves no trace on our side: the kernel
// names the thread whose job stuck, and nothing says what the job held. So each submit to the GPU's queue leaves a
// note here (who, and a line of what it held), the last HangNotes::kept of them, and each call that waits for the GPU
// says so while it waits. The watch reads both when a wait has lasted too long.
//
// Any thread; one mutex. No Vulkan, no clock of its own: the callers give the time (seconds, any steady clock).
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace pcr {

struct SubmitNote {
    uint64_t serial = 0; // (1 the first; 0: none)
    double at_s = 0;
    char who[16] = {};
    char what[200] = {};
};

class HangNotes {
public:
    static constexpr size_t kept = 64;
    static constexpr int waits_most = 8;

    // A submit to the GPU's queue: who made it (a thread's or a part's name) and what it held, a line
    void submitted(const char *who, const char *what, double now_s) {
        std::lock_guard<std::mutex> hold(lock_);
        SubmitNote &n = ring_[size_t(serial_ % kept)];
        n.serial = ++serial_;
        n.at_s = now_s;
        std::snprintf(n.who, sizeof(n.who), "%s", who);
        std::snprintf(n.what, sizeof(n.what), "%s", what);
    }

    // A call that waits for the GPU begins (where: a name that outlives the wait). The slot to end it with, or -1
    // when waits_most are waiting already (then it is not watched).
    int wait_begins(const char *where, double now_s) {
        std::lock_guard<std::mutex> hold(lock_);
        for (int i = 0; i < waits_most; i++) {
            if (waits_[size_t(i)].where == nullptr) {
                waits_[size_t(i)] = {where, now_s};
                return i;
            }
        }
        return -1;
    }
    void wait_ends(int slot) {
        if (slot < 0 || slot >= waits_most) {
            return;
        }
        std::lock_guard<std::mutex> hold(lock_);
        waits_[size_t(slot)] = {};
    }

    // The wait that has lasted longest: its seconds so far and where (0 and nullptr: nothing waits)
    double longest_wait(double now_s, const char **where) const {
        std::lock_guard<std::mutex> hold(lock_);
        double longest = 0;
        *where = nullptr;
        for (const Wait &w : waits_) {
            if (w.where != nullptr && (*where == nullptr || now_s - w.since_s > longest)) {
                longest = now_s - w.since_s;
                *where = w.where;
            }
        }
        return longest;
    }

    // The notes kept, oldest first
    std::vector<SubmitNote> notes() const {
        std::lock_guard<std::mutex> hold(lock_);
        std::vector<SubmitNote> out;
        for (size_t i = 0; i < kept; i++) {
            const SubmitNote &n = ring_[size_t((serial_ + i) % kept)];
            if (n.serial != 0) {
                out.push_back(n);
            }
        }
        return out;
    }

private:
    struct Wait {
        const char *where = nullptr;
        double since_s = 0;
    };
    mutable std::mutex lock_;
    std::array<SubmitNote, kept> ring_{};
    std::array<Wait, waits_most> waits_{};
    uint64_t serial_ = 0;
};

// A wait for the GPU, for as long as this lives (notes may be null: not watched)
class GpuWait {
public:
    GpuWait(HangNotes *notes, const char *where, double now_s)
        : notes_(notes), slot_(notes ? notes->wait_begins(where, now_s) : -1) {}
    ~GpuWait() {
        if (notes_) {
            notes_->wait_ends(slot_);
        }
    }
    GpuWait(const GpuWait &) = delete;
    GpuWait &operator=(const GpuWait &) = delete;

private:
    HangNotes *notes_;
    int slot_;
};

} // namespace pcr
