// The native draws' moves in the pictures between (docs/port/NATIVE_DRAW.md 3.5, P2-5), for the renderers that draw
// native draws without the native GS's code (the PC renderer: port/gs/pc/pcr_native.cpp): which part of the frame a
// draw is in (frame_interp.c's marks), where each model part was in the frame before, and how a picture between moves
// it. The native GS's gsn_native_draw.inc (nd_mark, nd_key, nd_remember, nd_model_flags, nd_between) and
// gsn_nd_strip.inc (nd_strip_between's pairing), the same maths; the native GS keeps its own copy, as with
// vu1_record.hpp.
//
// One thread: the renderer's.
#pragma once

#include <stdint.h>

#include <span>
#include <unordered_map>
#include <vector>

#include "render_internal.hpp"
#include "vu1_record.hpp"

// How a picture between moves a draw (the record's VU1_MOVE_* flags, obj and bt)
struct PartMove {
    int32_t flags = 0;               // VU1_MOVE_*
    float obj[4] = {};               // (VU1_MOVE_OBJ) the part's move from the frame before to this one, in the world
    Vu1Between model = {}, parent = {}; // (VU1_MOVE_MODEL) set A's (the part's matrix), and program 0x280's set B
};

// A model part's move from prev (the frame before) to cur (this frame), matrices of VU layout; with views, of the part
// in the eye's space (view_cur cur, view_prev prev: the arm). False when it is not a move between: a jump of 400 or
// more, a turn of nearly half way round, or a matrix with no rotation in it.
bool vu1_between(Vu1Between &out, const float *cur, const float *prev, const float *view_cur, const float *view_prev);

class PartMoves {
public:
    // A trail's points in a frame: X | Y << 16 (12.4) and RGBA each
    struct StripPoints {
        std::vector<uint32_t> xy, rgba;
    };

    // RD_ND_FRAME met (in_picture: in a picture between's walk of the frame's packets): the section starts again, and
    // the draws are counted again; a new serial outside a picture makes this frame's places the frame before's
    void frame(uint32_t serial, bool in_picture);
    // PORT_GS_REG_FRAME_INTERP's value (0x7D): its header marks say which part of the frame the draws after it are in
    void mark(uint64_t frame_interp_value);
    // A draw of program 0x000, 0x280, 0x5A0 or 0x700 met. Outside a picture between: its place kept for the next
    // frame's pictures (no move). In one: how it moves.
    PartMove draw(const RdDraw &d, const RdCamera &cam, bool in_picture);
    // A trail (RD_PROGRAM_GS_STRIP) met, its points this frame. Outside a picture: kept (nullptr). In one: the frame
    // before's points when paired, from[i] the partner of point i there; nullptr when not paired.
    const StripPoints *strip(const RdDraw &d, std::span<const uint32_t> xy, std::span<const uint32_t> rgba,
                             bool in_picture, std::vector<uint32_t> &from);

    // --stats: the draws moved by their own matrices, the trails paired
    uint64_t moved_models() const { return moved_models_; }
    uint64_t paired_strips() const { return paired_strips_; }

private:
    enum class Section : uint8_t { none, world, reflect, other, arm };
    // A draw's identity (RdDraw's kind and id), its section, and which draw of those in the frame it is
    struct Key {
        uint32_t k[6];
        bool operator==(const Key &o) const;
    };
    struct KeyHash {
        size_t operator()(const Key &key) const;
    };
    // Where a draw's part was: its matrix, its parent's (0x280's set B), the camera's world to view
    struct Place {
        float model[16], parent[16], view[16];
    };

    Key key_of(const RdDraw &d);
    int32_t model_flags(const Key &key, const RdDraw &d, const RdCamera &cam, Vu1Between *a, Vu1Between *b);

    Section section_ = Section::none;
    uint32_t serial_ = 0xFFFFFFFFu;
    std::unordered_map<Key, Place, KeyHash> now_, before_;
    std::unordered_map<Key, uint32_t, KeyHash> seen_;
    std::unordered_map<Key, StripPoints, KeyHash> strip_now_, strip_before_;
    uint64_t moved_models_ = 0, paired_strips_ = 0;
};
