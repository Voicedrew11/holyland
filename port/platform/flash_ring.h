// The size of the flash recorder's ring (present_vk.cpp, F8): the pictures it keeps on the GPU and their size.
//
// The ring keeps every picture presented for the last seconds asked for, so its size goes with the pictures a
// second: 2 seconds at 540 rows is 225 MB at 60 a second, and was 2500 MB with the frame rate unlocked to 500. It is
// held to FLASH_RING_MOST_BYTES: the pictures are kept smaller first (to FLASH_RING_FEWEST_ROWS, enough to see a
// flash by), and only then fewer of them, the seconds cut short.
//
// Header only, no state.
#pragma once

#include <cmath>
#include <cstdint>

constexpr uint64_t FLASH_RING_MOST_BYTES = 512ull << 20;
constexpr uint32_t FLASH_RING_FEWEST_ROWS = 270;
constexpr uint32_t FLASH_RING_MOST_LAYERS = 2048;

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
// picture's own height if that is less. No ring (layers 0) when any of them is not above 0.
inline FlashRingSize flash_ring_size(float seconds, int pictures_hz, int rows, int picture_w, int picture_h) {
    if (seconds <= 0 || rows <= 0 || picture_w <= 0 || picture_h <= 0) {
        return FlashRingSize();
    }
    const double asked = std::ceil(double(seconds) * double(pictures_hz < 60 ? 60 : pictures_hz));
    const uint32_t layers = asked < 2 ? 2 : asked > FLASH_RING_MOST_LAYERS ? FLASH_RING_MOST_LAYERS : uint32_t(asked);
    const uint32_t most_rows = uint32_t(rows > picture_h ? picture_h : rows);
    FlashRingSize size = flash_ring_rows(most_rows, picture_w, picture_h, layers);
    if (size.bytes() <= FLASH_RING_MOST_BYTES) {
        return size;
    }
    // smaller pictures: bytes go with the square of the rows
    const uint32_t fewest_rows = most_rows < FLASH_RING_FEWEST_ROWS ? most_rows : FLASH_RING_FEWEST_ROWS;
    uint32_t fitting = uint32_t(double(most_rows) * std::sqrt(double(FLASH_RING_MOST_BYTES) / double(size.bytes())));
    fitting = fitting < fewest_rows ? fewest_rows : fitting;
    size = flash_ring_rows(fitting, picture_w, picture_h, layers);
    while (size.bytes() > FLASH_RING_MOST_BYTES && size.height > fewest_rows) { // (the width's rounding)
        size = flash_ring_rows(size.height - 1, picture_w, picture_h, layers);
    }
    if (size.bytes() > FLASH_RING_MOST_BYTES) {
        // fewer pictures: the seconds cut short
        const uint64_t each = uint64_t(size.width) * size.height * 4;
        const uint64_t fit = FLASH_RING_MOST_BYTES / each;
        size.layers = fit < 2 ? 2 : uint32_t(fit);
    }
    return size;
}
