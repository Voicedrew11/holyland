// Opt-in VRAM call-site diagnostics for the scene renderer (KF4_VRAM_DIAG=1; off by default).
// Counts images created at each create_image call site, and, for the caches whose entries are explicitly
// let go (the decoded/sampled texture caches and the target-copy cache), also released, live and live bytes.
// "Released" means the app let go of its Granite handle; Granite defers the actual vkFreeMemory to a later
// frame context. Bytes are approximate (mip levels included, format bpp rounded to 1/2/4).
#ifndef SCN_VRAM_DIAG_H
#define SCN_VRAM_DIAG_H

#include <stdint.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace scn {
namespace vram_diag {

enum class Site : unsigned {
    target,
    stand_in,
    history,
    staged_upload,
    readback,
    gpu_texture,
    sampled_texture,
    target_copy,
    rect_copy,
    first_pass,
    count
};

inline const char *name(Site s) {
    switch (s) {
    case Site::target: return "target";
    case Site::stand_in: return "stand_in";
    case Site::history: return "history";
    case Site::staged_upload: return "staged_upload";
    case Site::readback: return "readback";
    case Site::gpu_texture: return "gpu_texture";
    case Site::sampled_texture: return "sampled_texture";
    case Site::target_copy: return "target_copy";
    case Site::rect_copy: return "rect_copy";
    case Site::first_pass: return "first_pass";
    default: return "?";
    }
}

struct Record {
    uint64_t created = 0, released = 0, live = 0, bytes = 0;
    bool tracked = false; // false: only creations are counted (no explicit release hook at this site)
};

struct State {
    std::mutex lock;
    Record at[unsigned(Site::count)];
    std::chrono::steady_clock::time_point last{};
    State() {
        for (unsigned i = 0; i < unsigned(Site::count); i++) {
            Site s = Site(i);
            at[i].tracked = s == Site::gpu_texture || s == Site::sampled_texture || s == Site::target_copy;
        }
    }
};

inline State &state() {
    static State s;
    return s;
}

inline bool enabled() {
    static const bool on = [] {
        const char *v = getenv("KF4_VRAM_DIAG");
        return v != nullptr && strcmp(v, "1") == 0;
    }();
    return on;
}

inline uint64_t approx_bytes(uint32_t w, uint32_t h, uint32_t levels, uint32_t bpp) {
    if (levels == 0) {
        levels = 1;
    }
    uint64_t n = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t lw = (w >> l) ? (w >> l) : 1, lh = (h >> l) ? (h >> l) : 1;
        n += uint64_t(lw) * lh * bpp;
    }
    return n;
}

inline void created(Site s, uint64_t bytes) {
    if (!enabled()) {
        return;
    }
    State &st = state();
    std::lock_guard<std::mutex> guard(st.lock);
    Record &r = st.at[unsigned(s)];
    r.created++;
    if (r.tracked) {
        r.live++;
        r.bytes += bytes;
    }
}

inline void released(Site s, uint64_t bytes) {
    if (!enabled()) {
        return;
    }
    State &st = state();
    std::lock_guard<std::mutex> guard(st.lock);
    Record &r = st.at[unsigned(s)];
    r.released++;
    if (r.live > 0) {
        r.live--;
    }
    r.bytes = r.bytes > bytes ? r.bytes - bytes : 0;
}

inline void tick() {
    if (!enabled()) {
        return;
    }
    State &st = state();
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> guard(st.lock);
    if (st.last.time_since_epoch().count() && now - st.last < std::chrono::seconds(1)) {
        return;
    }
    st.last = now;
    uint64_t live = 0, bytes = 0;
    for (const Record &r : st.at) {
        live += r.live;
        bytes += r.bytes;
    }
    fprintf(stderr, "[vram] t=%.1fs tracked live: %llu images, %.1f MiB (approx)\n",
            std::chrono::duration<double>(now.time_since_epoch()).count(), (unsigned long long)live,
            double(bytes) / (1024.0 * 1024.0));
    fprintf(stderr, "[vram] %-16s %10s %10s %8s %10s\n", "site", "created", "released", "live", "live MiB");
    for (unsigned i = 0; i < unsigned(Site::count); i++) {
        const Record &r = st.at[i];
        if (r.created == 0) {
            continue;
        }
        if (r.tracked) {
            fprintf(stderr, "[vram] %-16s %10llu %10llu %8llu %10.1f\n", name(Site(i)),
                    (unsigned long long)r.created, (unsigned long long)r.released, (unsigned long long)r.live,
                    double(r.bytes) / (1024.0 * 1024.0));
        } else {
            fprintf(stderr, "[vram] %-16s %10llu %10s %8s %10s\n", name(Site(i)),
                    (unsigned long long)r.created, "-", "-", "-");
        }
    }
}

} // namespace vram_diag
} // namespace scn

#endif
