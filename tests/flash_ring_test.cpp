// The flash recorder's ring size (port/platform/flash_ring.h): as asked while that fits, smaller pictures before
// fewer, never past FLASH_RING_MOST_BYTES.
#include "flash_ring.h"

#include <cstdio>
#include <initializer_list>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// 2 seconds at 60 a second, 540 rows of a 4:3 picture: as it always was (120 pictures of 720 x 540)
void as_asked_when_it_fits() {
    const FlashRingSize s = flash_ring_size(2.0f, 60, 540, 1920, 1440);
    CHECK(s.layers == 120 && s.width == 720 && s.height == 540);
    const FlashRingSize slow = flash_ring_size(2.0f, 30, 540, 1920, 1440); // (60 a second at least)
    CHECK(slow.layers == 120);
    const FlashRingSize small = flash_ring_size(2.0f, 60, 540, 640, 448); // (no taller than the picture)
    CHECK(small.height == 448 && small.width == 640);
}

// Unlocked to 500 a second, widescreen: all 1000 pictures still, smaller, where it was 2500 MB
void smaller_pictures_first() {
    const FlashRingSize s = flash_ring_size(2.0f, 500, 540, 2560, 1440);
    CHECK(s.layers == 1000);
    CHECK(s.height < 540 && s.height >= FLASH_RING_FEWEST_ROWS);
    CHECK(s.bytes() <= FLASH_RING_MOST_BYTES);
    CHECK(s.bytes() > FLASH_RING_MOST_BYTES * 9 / 10); // (and no smaller than it must be)
    CHECK(s.width % 2 == 0);
}

// More than fits at the fewest rows: the seconds cut short
void then_fewer_pictures() {
    const FlashRingSize s = flash_ring_size(4.0f, 500, 540, 2560, 1440);
    CHECK(s.height == FLASH_RING_FEWEST_ROWS);
    CHECK(s.layers < 2000 && s.layers >= 2);
    CHECK(s.bytes() <= FLASH_RING_MOST_BYTES);
    const FlashRingSize next = flash_ring_rows(s.height, 2560, 1440, s.layers + 1);
    CHECK(next.bytes() > FLASH_RING_MOST_BYTES); // (as many as fit)
}

void none_when_off() {
    CHECK(flash_ring_size(0.0f, 60, 540, 1920, 1440).layers == 0);
    CHECK(flash_ring_size(2.0f, 60, 0, 1920, 1440).layers == 0);
    CHECK(flash_ring_size(2.0f, 60, 540, 0, 1440).layers == 0);
    CHECK(flash_ring_size(2.0f, 60, 540, 1920, 0).layers == 0);
}

// Every rate and shape: within the most bytes and layers
void always_within() {
    for (int hz = 1; hz <= 1000; hz += 7) {
        for (int rows = 1; rows <= 2160; rows += 131) {
            for (float seconds : {0.01f, 1.0f, 2.0f, 10.0f}) {
                const FlashRingSize s = flash_ring_size(seconds, hz, rows, 3840, 1600);
                CHECK(s.layers >= 2 && s.layers <= FLASH_RING_MOST_LAYERS);
                CHECK(s.bytes() <= FLASH_RING_MOST_BYTES || s.layers == 2);
                CHECK(s.height >= 1 && s.width >= 2);
            }
        }
    }
}

// A GPU with little memory of its own: a sixteenth of its heap, within the least and the most
void held_to_the_heap() {
    CHECK(flash_ring_most_bytes(0, false) == FLASH_RING_MOST_BYTES);                // (not known: as before)
    CHECK(flash_ring_most_bytes(16ull << 30, false) == FLASH_RING_MOST_BYTES);      // (a 16 GB card)
    CHECK(flash_ring_most_bytes(8ull << 30, false) == FLASH_RING_MOST_BYTES);       // (an 8 GB card)
    CHECK(flash_ring_most_bytes(3318ull << 20, false) < 208ull << 20);              // (issue 261's 3 GB card)
    CHECK(flash_ring_most_bytes(2ull << 30, false) == 128ull << 20);                // (a 2 GB card)
    CHECK(flash_ring_most_bytes(512ull << 20, false) == FLASH_RING_LEAST_BYTES);     // (never below the least)
    CHECK(flash_ring_most_bytes(0, true) == FLASH_RING_MOST_BYTES_INTEGRATED);      // (integrated: the system's memory)
    CHECK(flash_ring_most_bytes(8ull << 30, true) == FLASH_RING_MOST_BYTES_INTEGRATED);
    const uint64_t most = flash_ring_most_bytes(8ull << 30, true);
    const FlashRingSize s = flash_ring_size(2.0f, 500, 540, 1920, 1080, most);
    CHECK(s.bytes() <= most);
    CHECK(s.height == FLASH_RING_FEWEST_ROWS); // (smaller pictures first, then fewer)
    CHECK(s.layers < 1000);
    CHECK(flash_ring_size(2.0f, 60, 540, 1920, 1080, most).bytes() <= most);
}

// Held to what the driver allocated, not the texels: issue 261's ring, 1035 pictures of 480 x 270 (512 MB of texels)
// allocated as 728 MB
void held_to_the_allocation() {
    const uint64_t most = 512ull << 20, allocated = 728ull << 20;
    const uint32_t layers = flash_ring_layers_allocated(1035, allocated, most);
    CHECK(layers < 1035);
    CHECK(uint64_t(layers) * (allocated / 1035) <= most); // (the same layout a picture)
    CHECK(flash_ring_layers_allocated(1035, most, most) == 1035); // (fits: as made)
    CHECK(flash_ring_layers_allocated(2, allocated, most) == 2);  // (never fewer than 2)
    CHECK(flash_ring_layers_allocated(100, 100ull << 30, 64ull << 20) == 2);
}

// The rate a ring was made for is kept while the rate found stays within a third of it, and for a while whatever it is
void rate_kept_near() {
    CHECK(flash_ring_keeps_rate(120, 120, 60.0));
    CHECK(flash_ring_keeps_rate(120, 150, 60.0));
    CHECK(flash_ring_keeps_rate(120, 95, 60.0));
    CHECK(!flash_ring_keeps_rate(120, 170, 60.0));
    CHECK(!flash_ring_keeps_rate(120, 80, 60.0));
    CHECK(flash_ring_keeps_rate(120, 959, 2.0)); // (a load's ceiling, soon after it was made)
    CHECK(flash_ring_keeps_rate(30, 60, 60.0));  // (both 60 at least)
    CHECK(flash_ring_keeps_rate(60, 75, 60.0));
    CHECK(!flash_ring_keeps_rate(0, 60, 60.0));  // (none made)
}

} // namespace

int main() {
    held_to_the_heap();
    held_to_the_allocation();
    rate_kept_near();
    as_asked_when_it_fits();
    smaller_pictures_first();
    then_fewer_pictures();
    none_when_off();
    always_within();
    if (failures == 0) {
        std::printf("flash_ring_test: ok\n");
    }
    return failures == 0 ? 0 : 1;
}
