// Dead writes in a walk drawn again and again (the pump's pictures between, pcr_canvas.cpp's draw_again): a batch
// whose every write is written over, at every pixel, before anything reads it need not be drawn. The game ends a
// frame with full-screen passes that the next pass, or the next picture's clear, makes moot: the destination alpha
// set to 0 and then written by the fade after it, Z cleared after the last draw that tests it, the frame's clear
// drawn twice. At 4x with two samples each is about 110 MB written for nothing.
//
// The walk is a list of steps in the order drawn. Each says, for the colour buffer and the Z buffer it draws into
// (keys: the buffer's base and width), which channels it may read there, which it may write, and which it writes at
// every pixel of its rectangle whatever was there (kills). The walk is drawn round and round on the same surfaces, so
// after the last step comes the first again; at the walk's end the picture is taken, which reads the colour.
//
// Every doubt is a read: a step that reads a channel it might not, or kills less than it does, only keeps a batch
// drawn that could have been left out.
//
// Header only, no state, no Vulkan.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace pcr {

enum : uint8_t { DEAD_RGB = 1, DEAD_ALPHA = 2, DEAD_Z = 4, DEAD_ALL = 7 };

struct WalkStep {
    static constexpr uint32_t no_buffer = 0xFFFFFFFFu;
    uint32_t colour = no_buffer, depth = no_buffer; // the buffers it draws into (a key each; no_buffer: none)
    int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;       // its rectangle in them, GS pixels, inclusive (its scissor)
    uint8_t reads = 0;  // channels of its own buffers it may read (DEAD_*)
    uint8_t writes = 0; // channels of its own buffers it may write
    uint8_t kills = 0;  // channels it writes at every pixel of its rectangle, whatever was there
    // channels it may read of every buffer, not only its own: a texture read from a surface (the colour and alpha of
    // any), an upload, a copy or a mark (everything)
    uint8_t reads_anywhere = 0;

    bool covers(const WalkStep &o) const {
        return x0 <= o.x0 && y0 <= o.y0 && x1 >= o.x1 && y1 >= o.y1;
    }
};

// For each step, whether what it writes is never read: written over by later steps (the first again after the last)
// or never looked at. The picture taken at the walk's end reads the colour of every buffer. A step that writes
// nothing is not called dead (it is not this file's to judge).
inline std::vector<bool> dead_writes(std::span<const WalkStep> walk) {
    const size_t n = walk.size();
    std::vector<bool> dead(n, false);
    for (size_t i = 0; i < n; i++) {
        const WalkStep &x = walk[i];
        uint8_t live = x.writes;
        if (live == 0) {
            continue;
        }
        bool read = false;
        for (size_t step = 1; step < n && live != 0 && !read; step++) {
            const size_t j = (i + step) % n;
            if (j == 0 && (live & DEAD_RGB) != 0) {
                read = true; // (the walk's end passed: the picture taken)
                break;
            }
            const WalkStep &y = walk[j];
            uint8_t reads = y.reads_anywhere;
            if (y.colour == x.colour) {
                reads |= y.reads & (DEAD_RGB | DEAD_ALPHA);
            }
            // (Z read through any Z buffer: the game's Z buffers of several widths lie over the same memory)
            reads |= y.reads & DEAD_Z;
            if ((live & reads) != 0) {
                read = true;
                break;
            }
            if (y.covers(x)) {
                if (y.colour == x.colour) {
                    live &= uint8_t(~(y.kills & (DEAD_RGB | DEAD_ALPHA)));
                }
                if (y.depth == x.depth) {
                    live &= uint8_t(~(y.kills & DEAD_Z));
                }
            }
        }
        // (still live after the whole round, or the walk's only step: the colour is the picture's; alpha and Z nothing
        // outside the walk looks at)
        if (!read && (live & DEAD_RGB) != 0) {
            read = true;
        }
        dead[i] = !read;
    }
    return dead;
}

} // namespace pcr
