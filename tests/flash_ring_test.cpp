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

} // namespace

int main() {
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
