// The frame's own walk as its pictures between's walk (docs/port/PC_RENDERER.md, "A frame walked once"): what
// frame_interp.c's replay_pending does to the frame's packets when it hands them over on the GPU's way, as rules on
// what the PC renderer's walk of the frame's own packets made (pcr_canvas.h's as_picture_walk). The stream's walk
// draws into the buffer shown where the frame drew into its own (FRAME, TEX0 and BITBLTBUF), asks for the uploads
// again, leaves out the read-backs and the copies, and reads the kept frame shown where a draw's TEX0 names it.
// What the rules do not make as the stream does (a draw that reads the buffer drawn other than as a surface's pixels, a
// copy from or into it, an upload into it: the stream's own choices there are frame_interp.c's) is refused, and the
// frame walks its stream.
//
// Plain functions of the registers: no state, any thread.
#pragma once

#include <cstdint>

#include "pcr_gif.h"

namespace pcr {

// GsnInterpFrame's account of the stream (gsn.h)
struct FrameTargets {
    uint32_t draw_fbp = 0, shown_fbp = 0; // (FBP: in pages of 32 blocks)
    bool uploads_asked_again = false;     // the uploads into anything but draw_fbp asked for again (0x7C)
    bool shown_kept = false;              // the frame shown kept (0xF2): a draw whose TEX0 names it reads the copy
};

// Why a frame's own walk was not made its pictures' walk (and the frame's stream was walked)
enum class FrameWalkRefused : uint8_t {
    none,
    not_kept,         // no walk of the stretch kept, or not whole (a picture between in it, or the canvas's)
    not_the_frame,    // the stretch's bytes or kicks are not the frame's
    trail_paired,     // a trail a picture moves on the CPU (NativeDraws::trail_paired)
    reads_drawn,      // a draw textured from the buffer drawn, not from its surface's pixels
    copies_drawn,     // a local copy from or into the buffer drawn
    uploads_drawn,    // an upload into the buffer drawn
    native_unpaired,  // a native batch without its picture's record
    marks,            // a mark of a picture between's (0xF0 to 0xF5) or an upload asked again: not the game's own
    count
};

// What the stream does with one of the frame's transfers
enum class TransferInPicture : uint8_t { as_is, asked_again, left_out, refused };

inline constexpr uint32_t frame_walk_bits(uint64_t v, unsigned at, unsigned n) {
    return uint32_t((v >> at) & ((uint64_t(1) << n) - 1));
}

// FRAME_n: its FBP the buffer shown where it is the buffer drawn (ad_write's GS_FRAME_1)
inline constexpr uint64_t frame_in_picture(uint64_t frame, const FrameTargets &t) {
    return frame_walk_bits(frame, 0, 9) == t.draw_fbp ? (frame & ~uint64_t(0x1FF)) | t.shown_fbp : frame;
}

// TEX0_n: its TBP0 the buffer shown where it is the buffer drawn (ad_write's GS_TEX0_1)
inline constexpr uint64_t tex0_in_picture(uint64_t tex0, const FrameTargets &t) {
    return frame_walk_bits(tex0, 0, 14) == t.draw_fbp * 32 ? (tex0 & ~uint64_t(0x3FFF)) | (t.shown_fbp * 32) : tex0;
}

// A draw with this TEX0 reads the buffer drawn (refused when textured, but from the surface's pixels)
inline constexpr bool tex0_reads_drawn(uint64_t tex0, const FrameTargets &t) {
    return frame_walk_bits(tex0, 0, 14) == t.draw_fbp * 32;
}

// A draw with this TEX0 reads the kept frame shown in the stream (between 0xF4 and 0xF5), when textured
inline constexpr bool tex0_reads_kept(uint64_t tex0, const FrameTargets &t) {
    return t.shown_kept && frame_walk_bits(tex0, 0, 14) == t.shown_fbp * 32;
}

// A transfer of TRXDIR direction (0 upload, 1 read-back, 2 local copy) as the stream has it (ad_write's GS_TRXDIR,
// not the exact restore): an upload into anything but the buffer drawn asked for again (or as it is), a read-back
// and a copy into anything but the buffer drawn left out; a copy from the buffer drawn (the stream's choice is
// frame_interp.c's Flat copies), a copy into it, an upload into it (retargeted) refused
inline constexpr TransferInPicture transfer_in_picture(uint32_t direction, const TransferRegisters &r,
                                                       const FrameTargets &t) {
    uint32_t sbp = frame_walk_bits(r.bitbltbuf, 0, 14), dbp = frame_walk_bits(r.bitbltbuf, 32, 14);
    uint32_t drawn = t.draw_fbp * 32;
    switch (direction) {
    case 0:
        if (dbp == drawn) {
            return TransferInPicture::refused;
        }
        return t.uploads_asked_again ? TransferInPicture::asked_again : TransferInPicture::as_is;
    case 1:
        return TransferInPicture::left_out;
    case 2:
        if (sbp == drawn || dbp == drawn) {
            return TransferInPicture::refused;
        }
        return TransferInPicture::left_out;
    default:
        return TransferInPicture::as_is;
    }
}

} // namespace pcr
