// A texture's smaller levels made on the PC (mip_chain.h)
#include "mip_chain.h"

#include <algorithm>

namespace pcr {

namespace {

uint32_t channel(uint32_t texel, uint32_t c) {
    return (texel >> (8 * c)) & 0xFFu;
}

// The texel of the level below at (x, y): the 2 x 2 above it (1 x 2, 2 x 1 at a side of 1), the colour weighted by alpha
uint32_t averaged(std::span<const uint32_t> above, uint32_t width, uint32_t height, uint32_t x, uint32_t y) {
    uint32_t x0 = std::min(2 * x, width - 1), x1 = std::min(2 * x + 1, width - 1);
    uint32_t y0 = std::min(2 * y, height - 1), y1 = std::min(2 * y + 1, height - 1);
    const uint32_t four[4] = {above[size_t(y0) * width + x0], above[size_t(y0) * width + x1],
                              above[size_t(y1) * width + x0], above[size_t(y1) * width + x1]};
    uint32_t alpha_sum = 0;
    uint32_t weighted[3] = {}, plain[3] = {};
    for (uint32_t t : four) {
        uint32_t a = channel(t, 3);
        alpha_sum += a;
        for (uint32_t c = 0; c < 3; c++) {
            weighted[c] += channel(t, c) * a;
            plain[c] += channel(t, c);
        }
    }
    uint32_t out = ((alpha_sum + 2) / 4) << 24;
    for (uint32_t c = 0; c < 3; c++) {
        uint32_t v = alpha_sum ? (weighted[c] + alpha_sum / 2) / alpha_sum : (plain[c] + 2) / 4;
        out |= v << (8 * c);
    }
    return out;
}

} // namespace

uint32_t mip_chain_levels(uint32_t width, uint32_t height) {
    uint32_t levels = 1;
    while (width > 1 || height > 1) {
        width = std::max(width / 2, 1u);
        height = std::max(height / 2, 1u);
        levels++;
    }
    return levels;
}

std::vector<uint32_t> mip_chain_below(std::span<const uint32_t> texels, uint32_t width, uint32_t height) {
    std::vector<uint32_t> out;
    if (width == 0 || height == 0 || (width == 1 && height == 1) || texels.size() < size_t(width) * height) {
        return out;
    }
    size_t above_at = 0;
    bool above_in_out = false;
    while (width > 1 || height > 1) {
        uint32_t w = std::max(width / 2, 1u), h = std::max(height / 2, 1u);
        size_t at = out.size();
        out.resize(at + size_t(w) * h);
        // (the level above: the caller's for the first, out's own after; out may have moved in the resize)
        std::span<const uint32_t> above = above_in_out ? std::span<const uint32_t>(out).subspan(above_at)
                                                       : texels.first(size_t(width) * height);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                out[at + size_t(y) * w + x] = averaged(above, width, height, x, y);
            }
        }
        above_at = at;
        above_in_out = true;
        width = w;
        height = h;
    }
    return out;
}

} // namespace pcr
