// The scene renderer's GPU texture-image cache budget (VRAM bytes). Kept free of Vulkan/Granite types so the
// policy can be tested without a device or game assets (tests/scn_texture_budget_test.cpp).
#ifndef SCN_TEXTURE_BUDGET_H
#define SCN_TEXTURE_BUDGET_H

#include <stdint.h>

namespace scn {

inline constexpr uint64_t texture_budget_least_bytes = uint64_t(256) << 20;
inline constexpr uint64_t texture_budget_most_bytes = uint64_t(1) << 30;

// What KF4_SCN_TEXTURE_CAP_MIB asks: unset (or not a number) keeps the default, "0" means no cap (A/B), N MiB a cap
// of its own.
struct TextureCapOverride {
    bool set = false;
    uint64_t mib = 0;
};

inline TextureCapOverride texture_cap_override(const char *text) {
    TextureCapOverride o;
    if (text == nullptr || *text < '0' || *text > '9') {
        return o;
    }
    o.set = true;
    for (; *text >= '0' && *text <= '9'; text++) {
        o.mib = o.mib > (UINT64_MAX >> 20) / 10 ? UINT64_MAX : o.mib * 10 + uint64_t(*text - '0');
    }
    return o;
}

// A quarter of the device-local heap's budget, held between 256 MiB and 1 GiB; 0 is no cap. One map's textures at
// high settings (native scale at 1440p, finer textures) take about 260 MiB, and seamless scroll draws two maps at
// once, so the ceiling leaves room for both on a big card. The floor is what an integrated GPU, whose own heap is
// small but which shares the PC's memory, was measured with (issue 259). The cap only bounds what the cache keeps:
// a texture drawn in this vblank or the last is never let go (evict_textures in scn_build.cpp), so the movie's
// pictures, each used once, go a vblank later whatever the cap.
inline uint64_t texture_cap_bytes(uint64_t heap_budget_bytes, TextureCapOverride over) {
    if (over.set) {
        return over.mib > (UINT64_MAX >> 20) ? UINT64_MAX : over.mib << 20;
    }
    if (heap_budget_bytes == 0) {
        return texture_budget_least_bytes;
    }
    const uint64_t quarter = heap_budget_bytes / 4;
    return quarter < texture_budget_least_bytes  ? texture_budget_least_bytes
           : quarter > texture_budget_most_bytes ? texture_budget_most_bytes
                                                 : quarter;
}

struct ByteTracker {
    uint64_t live = 0;
    void add(uint64_t bytes) { live += bytes; }
    void remove(uint64_t bytes) { live = live > bytes ? live - bytes : 0; }
    bool over(uint64_t cap) const { return cap != 0 && live > cap; }
};

} // namespace scn

#endif
