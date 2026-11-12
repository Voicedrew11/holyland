// A native draw of a screen pass (RD_PROGRAM_PASS: game/native_draw/nd_passes.c, port/include/port/render.h's RdPass)
// as the GIF packets the PS2 path's would have been, for the renderers that draw native draws (the PC renderer:
// port/gs/pc/pcr_native.cpp). GameLoop_DrawFrame's passes (the brightness and the tint, the fills and the fade, the
// flicker filter, the frame shown before blended in, the view-turn shift, haze and the wobble grid) are register lists
// and sprites that VU1's program 0x276 passes on; here they are the same register writes in the same order, and the
// same primitives, so the GS's state and what is drawn after are the PS2 path's. The flat fills are one sprite in place
// of the game's ten 64-pixel strips (one colour: the same pixels); the passes that read the frame keep the game's
// primitives (the brightness and the tint, the flicker filter, the view-turn shift, the wobble grid).
//
// Pictures between: the PS2 path's passes are 2D, which frame_interp.c pairs with the frame before's (flat_pair,
// items_pair) and draws t of the way from there (port/render/shaders/gif_moves.h's kick_now): a fill's colour, the
// wobble grid's texels, the view-turn shift's strips, and the frame shown before blended in by A^t. A native pass is
// paired by its identity instead (the pass, its call site and its count in the frame), with the same pass of the frame
// before (PassMoves), and its packets are made with those moves (PassMove): the same numbers as gif_moves.h's.
//
// One thread: the renderer's.
#pragma once

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "part_moves.hpp"
#include "render_internal.hpp"

// GIF packets: PACKED tags (the tag's two words) and their qwords (two words each), in order
struct PassStream {
    std::vector<uint64_t> q;
    void tag(uint64_t low, uint64_t regs) {
        q.push_back(low);
        q.push_back(regs);
    }
    void words(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        q.push_back(uint64_t(a) | uint64_t(b) << 32);
        q.push_back(uint64_t(c) | uint64_t(d) << 32);
    }
    void ad(uint32_t reg, uint64_t value) {
        q.push_back(value);
        q.push_back(reg);
    }
};

// PassMove's mode
enum : uint32_t {
    PASS_MOVE_RGBA = 0x1,   // the corners' colour t of the way from the frame before's (prev_rgba)
    PASS_MOVE_PREV = 0x2,   // the frame shown before blended in by A^t (A t of the way from prev_rgba's when PAIRED)
    PASS_MOVE_PAIRED = 0x4,
    PASS_MOVE_GRID = 0x8,   // the wobble's cells that pair (cells) t of the way from the frame before's, colour and texels
    PASS_MOVE_SHIFT = 0x10, // the view-turn shift's strips that pair (strips) t of the way from the frame before's
};

// How a picture between draws a pass (none outside one)
struct PassMove {
    uint32_t mode = 0;
    uint32_t prev_rgba = 0; // R | G << 8 | B << 16 | A << 24
    uint32_t strips = 0;    // (PASS_MOVE_SHIFT) the strips that pair
    uint32_t cells[3] = {}; // (PASS_MOVE_GRID) the cells that pair, i * 10 + j
    RdPass before = {};     // (PASS_MOVE_GRID, PASS_MOVE_SHIFT) the frame before's pass
    float t = 1.0f;         // the picture's time
};

// The pass's packets, as the PS2 path's draw it at the picture's time (move: PassMove() outside a picture between)
void pass_stream(const RdPass &p, const PassMove &move, PassStream &out);

class PassMoves {
public:
    // RD_ND_FRAME met (as PartMoves::frame): a new serial outside a picture makes this frame's passes the frame before's
    void frame(uint32_t serial, PartWalk walk);
    // A pass met: kept for the next frame's pictures (the frame's walks), and how a picture between at t draws it
    // (picture, frame_kept). tex0: TEX0_1 as the pass is met (the wobble's texture, the game's own write before it);
    // shown_kept: the frame shown is kept for the pictures between (what the frame shown before's pass reads)
    PassMove pass(const RdPass &p, uint64_t tex0, bool shown_kept, PartWalk walk, float t);

private:
    struct Key {
        uint32_t pass, site, count;
        bool operator==(const Key &o) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key &k) const { return size_t(k.pass) * 0x9E3779B1u ^ size_t(k.site) << 16 ^ k.count; }
    };
    uint32_t serial_ = 0xFFFFFFFFu;
    std::unordered_map<Key, RdPass, KeyHash> now_, before_;
    std::unordered_map<uint64_t, uint32_t> seen_; // (a pass and site's count in the frame)
};
