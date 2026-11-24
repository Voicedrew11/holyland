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
    const TextureCapOverride none;
    // A 3 GB device (VK_EXT_memory_budget reports what is left of it): a quarter.
    CHECK(texture_cap_bytes(2800 * mib, none) == 700 * mib);
    // A large-VRAM device: held to 1 GiB, so it cannot grow without bound.
    CHECK(texture_cap_bytes(uint64_t(24) << 30, none) == 1024 * mib);
    // A small heap (an integrated GPU's own memory): not under 256 MiB.
    CHECK(texture_cap_bytes(256 * mib, none) == 256 * mib);
    // No budget extension: the floor.
    CHECK(texture_cap_bytes(0, none) == 256 * mib);
}

void overridden() {
    const uint64_t mib = uint64_t(1) << 20;
    // Unset or not a number: the default.
    CHECK(!texture_cap_override(nullptr).set);
    CHECK(!texture_cap_override("").set);
    CHECK(!texture_cap_override("off").set);
    // A number of MiB, whatever the budget.
    CHECK(texture_cap_bytes(2800 * mib, texture_cap_override("128")) == 128 * mib);
    // "0": no cap at all, which ByteTracker::over treats as never over.
    const TextureCapOverride zero = texture_cap_override("0");
    CHECK(zero.set && zero.mib == 0);
    CHECK(texture_cap_bytes(2800 * mib, zero) == 0);
    // Too large to hold in bytes: as large as can be, not wrapped round.
    CHECK(texture_cap_bytes(0, texture_cap_override("99999999999999999999999")) == UINT64_MAX);
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
    overridden();
    accounting();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("scn_texture_budget: OK\n");
    return 0;
}
