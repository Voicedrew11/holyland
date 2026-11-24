// The size of the flash recorder's ring (present_vk.cpp, F8): the pictures it keeps on the GPU and their size.
//
// The ring keeps every picture presented for the last seconds asked for, so its size goes with the pictures a
// second: 2 seconds at 540 rows is 225 MB at 60 a second, and was 2500 MB with the frame rate unlocked to 500. It is
// held to FLASH_RING_MOST_BYTES (less on a GPU with little memory of its own: flash_ring_most_bytes): the pictures
// are kept smaller first (to FLASH_RING_FEWEST_ROWS, enough to see a flash by), and only then fewer of them, the
// seconds cut short.
//
// Header only, no state.
#pragma once

#include <cmath>
#include <cstdint>

constexpr uint64_t FLASH_RING_MOST_BYTES = 512ull << 20;
constexpr uint32_t FLASH_RING_FEWEST_ROWS = 270;
constexpr uint32_t FLASH_RING_MOST_LAYERS = 2048;
constexpr uint64_t FLASH_RING_LEAST_BYTES = 64ull << 20; // (the most never held below this, however small the GPU)

constexpr uint64_t FLASH_RING_MOST_BYTES_INTEGRATED = 128ull << 20;

// The most bytes of the ring on a GPU whose device-local heap is heap_bytes (0: not known): a sixteenth of it, at most
// FLASH_RING_MOST_BYTES (FLASH_RING_MOST_BYTES_INTEGRATED on an integrated GPU) and at least FLASH_RING_LEAST_BYTES.
// An integrated GPU's memory is the system's, and its own carve-out small (512 MB on a Ryzen 5 5625U, though RADV
// names a heap of several GB): a 512 MB ring there (776 MB as allocated) took more than the carve-out by itself
// (2026-11-18). A sixteenth, not a quarter: on a 3 GB GTX 1050 a quarter let the ring take 728 MB as allocated,
// with the desktop holding 900 MB, and the game ran out of memory and showed black (issue 261). A card of 8 GB or
// more keeps the most.
inline uint64_t flash_ring_most_bytes(uint64_t heap_bytes, bool integrated) {
    const uint64_t most = integrated ? FLASH_RING_MOST_BYTES_INTEGRATED : FLASH_RING_MOST_BYTES;
    if (heap_bytes == 0) {
        return most;
    }
    const uint64_t share = heap_bytes / 16;
    return share > most ? most : share < FLASH_RING_LEAST_BYTES ? FLASH_RING_LEAST_BYTES : share;
}

// The pictures a ring made with `layers` keeps in most_bytes when the driver allocated allocated_bytes for it: the
// driver lays each picture out in its own way (NVIDIA: 728 MB for a ring of 512 MB of texels, 1035 pictures of 480 x
// 270), so the most is held to what is allocated, not to the texels. At least 2.
inline uint32_t flash_ring_layers_allocated(uint32_t layers, uint64_t allocated_bytes, uint64_t most_bytes) {
    if (allocated_bytes <= most_bytes || layers <= 2) {
        return layers;
    }
    const uint64_t fit = uint64_t(layers) * most_bytes / allocated_bytes;
    return fit < 2 ? 2 : uint32_t(fit);
}

constexpr double FLASH_RING_REMAKE_SECONDS = 10.0;

// Whether a ring made for made_hz pictures a second, made_seconds_ago, is kept at now_hz: the ring is made again for
// the rate (which waits for the GPU to be idle, and frees and allocates hundreds of MB) only when the rate has moved
// by more than a third, and not within FLASH_RING_REMAKE_SECONDS of the last time. Unlocked's rate is found as it
// plays: it is its ceiling (959) while nothing is shown, through a load or a teleport, and about what is shown after,
// and the ring was made again at each (2026-11-18: three times in 20 s of play).
inline bool flash_ring_keeps_rate(int made_hz, int now_hz, double made_seconds_ago) {
    if (made_hz <= 0 || now_hz <= 0) {
        return made_hz == now_hz;
    }
    const int made = made_hz < 60 ? 60 : made_hz, now = now_hz < 60 ? 60 : now_hz; // (60 a second at least, as below)
    return (now * 3 <= made * 4 && made * 3 <= now * 4) || made_seconds_ago < FLASH_RING_REMAKE_SECONDS;
}

struct FlashRingSize {
    uint32_t width = 0, height = 0; // of each picture kept, in pixels (RGBA8); the width even
    uint32_t layers = 0;            // pictures kept; 0: no ring
    uint64_t bytes() const { return uint64_t(width) * height * 4 * layers; }
};

// A ring's pictures of this many rows, shaped as the picture presented (picture_w x picture_h)
inline FlashRingSize flash_ring_rows(uint32_t rows, int picture_w, int picture_h, uint32_t layers) {
    FlashRingSize size;
    size.height = rows;
    size.width = (uint32_t(std::lround(double(rows) * picture_w / picture_h)) + 1u) & ~1u;
    size.layers = layers;
    return size;
}

// The ring for the last `seconds` of pictures at pictures_hz a second (60 at least), each kept at `rows` rows or the
// picture's own height if that is less, in most_bytes (flash_ring_most_bytes). No ring (layers 0) when any of them is
// not above 0.
inline FlashRingSize flash_ring_size(float seconds, int pictures_hz, int rows, int picture_w, int picture_h,
                                     uint64_t most_bytes = FLASH_RING_MOST_BYTES) {
    if (seconds <= 0 || rows <= 0 || picture_w <= 0 || picture_h <= 0) {
        return FlashRingSize();
    }
    const double asked = std::ceil(double(seconds) * double(pictures_hz < 60 ? 60 : pictures_hz));
    const uint32_t layers = asked < 2 ? 2 : asked > FLASH_RING_MOST_LAYERS ? FLASH_RING_MOST_LAYERS : uint32_t(asked);
    const uint32_t most_rows = uint32_t(rows > picture_h ? picture_h : rows);
    FlashRingSize size = flash_ring_rows(most_rows, picture_w, picture_h, layers);
    if (size.bytes() <= most_bytes) {
        return size;
    }
    // smaller pictures: bytes go with the square of the rows
    const uint32_t fewest_rows = most_rows < FLASH_RING_FEWEST_ROWS ? most_rows : FLASH_RING_FEWEST_ROWS;
    uint32_t fitting = uint32_t(double(most_rows) * std::sqrt(double(most_bytes) / double(size.bytes())));
    fitting = fitting < fewest_rows ? fewest_rows : fitting;
    size = flash_ring_rows(fitting, picture_w, picture_h, layers);
    while (size.bytes() > most_bytes && size.height > fewest_rows) { // (the width's rounding)
        size = flash_ring_rows(size.height - 1, picture_w, picture_h, layers);
    }
    if (size.bytes() > most_bytes) {
        // fewer pictures: the seconds cut short
        const uint64_t each = uint64_t(size.width) * size.height * 4;
        const uint64_t fit = most_bytes / each;
        size.layers = fit < 2 ? 2 : uint32_t(fit);
    }
    return size;
}
