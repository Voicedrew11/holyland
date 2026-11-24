// The scene renderer's texture-image VRAM cap (port/gs/scene/scn_texture_budget.h): the cap comes from the real
// heap budget (or an override), and the accounting never underflows when entries are let go.
#include "scn_texture_budget.h"

#include <cstdio>

using namespace scn;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

void capped_by_budget() {
    const uint64_t mib = uint64_t(1) << 20;
    // A 2 GB device (VK_EXT_memory_budget reports just under 2048 MiB): use a quarter, not the default.
    CHECK(texture_cap_bytes(1984 * mib, 0) == 496 * mib);
    // A large-VRAM device: the absolute cap wins, so it cannot grow without bound.
    CHECK(texture_cap_bytes(uint64_t(24) << 30, 0) == 512 * mib);
    // No budget extension: fall back to the default.
    CHECK(texture_cap_bytes(0, 0) == 512 * mib);
    // An override (0 disables the cap, for A/B).
    CHECK(texture_cap_bytes(1984 * mib, 128) == 128 * mib);
    CHECK(texture_cap_bytes(1984 * mib, 0) != 0);
}

void accounting() {
    ByteTracker t;
    const uint64_t mib = uint64_t(1) << 20;
    CHECK(!t.over(100 * mib));
    t.add(40 * mib);
    t.add(70 * mib);
    CHECK(t.live == 110 * mib && t.over(100 * mib));
    t.remove(70 * mib);
    CHECK(t.live == 40 * mib && !t.over(100 * mib));
    // A release of more than is tracked clamps to zero (a cache let go wholesale).
    t.remove(100 * mib);
    CHECK(t.live == 0 && !t.over(100 * mib));
    // A cap of 0 disables eviction regardless of live bytes.
    t.add(1000 * mib);
    CHECK(!t.over(0));
}

} // namespace

int main() {
    capped_by_budget();
    accounting();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("scn_texture_budget: OK\n");
    return 0;
}
