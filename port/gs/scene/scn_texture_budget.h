// The scene renderer's GPU texture-image cache budget (VRAM bytes). Kept free of Vulkan/Granite types so the
// policy can be tested without a device or game assets (tests/scn_texture_budget_test.cpp).
#ifndef SCN_TEXTURE_BUDGET_H
#define SCN_TEXTURE_BUDGET_H

#include <stdint.h>

namespace scn {

inline constexpr uint64_t texture_budget_default_bytes = uint64_t(512) << 20;

// A quarter of the device-local heap's budget, at most 512 MiB. The PS2's own VRAM is 4 MiB: the only reason a
// decoded texture image gets large is a broad TEX0 (the opening movie declares 1024x1024 for a 640x448 picture),
// so a few hundred MiB is already far more than a legitimate set of textures. A 0 override means "no cap" (A/B).
inline uint64_t texture_cap_bytes(uint64_t heap_budget_bytes, uint64_t override_mib) {
    if (override_mib != 0) {
        return override_mib > (UINT64_MAX >> 20) ? UINT64_MAX : override_mib << 20;
    }
    if (heap_budget_bytes == 0) {
        return texture_budget_default_bytes;
    }
    uint64_t quarter = heap_budget_bytes / 4;
    return quarter < texture_budget_default_bytes ? quarter : texture_budget_default_bytes;
}

struct ByteTracker {
    uint64_t live = 0;
    void add(uint64_t bytes) { live += bytes; }
    void remove(uint64_t bytes) { live = live > bytes ? live - bytes : 0; }
    bool over(uint64_t cap) const { return cap != 0 && live > cap; }
};

} // namespace scn

#endif
