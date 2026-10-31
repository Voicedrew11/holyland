// The frame's own walk as its pictures between's walk (port/gs/pc/pcr_frame_walk.h): the rules that frame_interp.c's
// ad_write makes on FRAME, TEX0, BITBLTBUF and TRXDIR in REPLAY, with the drawn and shown buffers at concrete pages
// (FBP is in pages of 32 blocks; TBP, SBP and DBP are in blocks). Compile-time checks first, then the same rules at
// run time on whole register values, so every field that must be kept is seen kept.
#include "pcr_frame_walk.h"

#include <cstdint>
#include <cstdio>

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

constexpr uint32_t kDrawFbp = 0x70;  // the buffer drawn: pages 0x70 (blocks 0xE00)
constexpr uint32_t kShownFbp = 0xA0; // the buffer shown in the picture between: pages 0xA0 (blocks 0x1400)
constexpr uint32_t kDrawnBlocks = kDrawFbp * 32;
constexpr uint32_t kShownBlocks = kShownFbp * 32;
constexpr FrameTargets kTargets{.draw_fbp = kDrawFbp, .shown_fbp = kShownFbp};
constexpr FrameTargets kTargetsKept{.draw_fbp = kDrawFbp, .shown_fbp = kShownFbp, .shown_kept = true};
constexpr FrameTargets kTargetsAsked{.draw_fbp = kDrawFbp, .shown_fbp = kShownFbp, .uploads_asked_again = true};

// FRAME_n: FBP 0-8, FBW 16-21, PSM 24-29, FBMSK 32-63
constexpr uint64_t frame_reg(uint32_t fbp, uint32_t fbw, uint32_t psm, uint32_t mask) {
    return uint64_t(fbp) | uint64_t(fbw) << 16 | uint64_t(psm) << 24 | uint64_t(mask) << 32;
}

// TEX0_n: TBP0 0-13, TBW 14-19, PSM 20-25, TW 26-29, TH 30-33, TCC 34, TFX 35-36, CBP 37-50, CPSM 51-54, CSM 55,
// CSA 56-60, CLD 61-63
constexpr uint64_t tex0_reg(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th, uint32_t cbp,
                            uint32_t cld) {
    return uint64_t(tbp) | uint64_t(tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tw) << 26 | uint64_t(th) << 30 |
           uint64_t(1) << 34 | uint64_t(2) << 35 | uint64_t(cbp) << 37 | uint64_t(0xA) << 51 | uint64_t(1) << 55 |
           uint64_t(7) << 56 | uint64_t(cld) << 61;
}

// BITBLTBUF: SBP 0-13, SBW 16-21, SPSM 24-26, DBP 32-45, DBW 48-53, DPSM 56-58
constexpr uint64_t bitbltbuf_reg(uint32_t sbp, uint32_t dbp) {
    return uint64_t(sbp) | uint64_t(5) << 16 | uint64_t(0) << 24 | uint64_t(dbp) << 32 | uint64_t(6) << 48 |
           uint64_t(0) << 56;
}

// ---- Compile-time checks: the rules on constants -----------------------------------------------------------------

static_assert(frame_walk_bits(0xABCD, 4, 8) == 0xBC);
static_assert(frame_walk_bits(~uint64_t(0), 0, 32) == 0xFFFFFFFFu);
static_assert(frame_in_picture(frame_reg(kDrawFbp, 10, 0, 0), kTargets) == frame_reg(kShownFbp, 10, 0, 0));
static_assert(frame_in_picture(frame_reg(kDrawFbp + 1, 10, 0, 0), kTargets) == frame_reg(kDrawFbp + 1, 10, 0, 0));
static_assert(tex0_in_picture(kDrawnBlocks, kTargets) == kShownBlocks);
static_assert(tex0_in_picture(kDrawnBlocks + 1, kTargets) == kDrawnBlocks + 1);
static_assert(tex0_reads_drawn(kDrawnBlocks, kTargets));
static_assert(!tex0_reads_drawn(kShownBlocks, kTargets));
static_assert(!tex0_reads_kept(kShownBlocks, kTargets));
static_assert(tex0_reads_kept(kShownBlocks, kTargetsKept));
static_assert(!tex0_reads_kept(kDrawnBlocks, kTargetsKept));
static_assert(transfer_in_picture(1, TransferRegisters{}, kTargets) == TransferInPicture::left_out);
static_assert(transfer_in_picture(3, TransferRegisters{}, kTargets) == TransferInPicture::as_is);
static_assert(transfer_in_picture(0, TransferRegisters{.bitbltbuf = bitbltbuf_reg(0, kDrawnBlocks)}, kTargets) ==
              TransferInPicture::refused);
static_assert(transfer_in_picture(0, TransferRegisters{.bitbltbuf = bitbltbuf_reg(0, 0x40)}, kTargetsAsked) ==
              TransferInPicture::asked_again);
static_assert(transfer_in_picture(2, TransferRegisters{.bitbltbuf = bitbltbuf_reg(kDrawnBlocks, 0x40)}, kTargets) ==
              TransferInPicture::refused);
static_assert(transfer_in_picture(2, TransferRegisters{.bitbltbuf = bitbltbuf_reg(0x40, kDrawnBlocks)}, kTargets) ==
              TransferInPicture::refused);
static_assert(transfer_in_picture(2, TransferRegisters{.bitbltbuf = bitbltbuf_reg(0x40, 0x80)}, kTargets) ==
              TransferInPicture::left_out);

// ---- Run time: whole register values ------------------------------------------------------------------------------

void test_frame_in_picture() {
    uint64_t frame = frame_reg(kDrawFbp, 10, 0x1B, 0xFFFF0000u);
    uint64_t moved = frame_in_picture(frame, kTargets);
    CHECK(moved == frame_reg(kShownFbp, 10, 0x1B, 0xFFFF0000u));
    CHECK((moved & 0x1FF) == kShownFbp);
    CHECK(((moved >> 16) & 0x3F) == 10);  // FBW kept
    CHECK(((moved >> 24) & 0x3F) == 0x1B); // PSM kept
    CHECK((moved >> 32) == 0xFFFF0000u);  // FBMSK kept

    uint64_t other = frame_reg(kDrawFbp + 1, 10, 0x1B, 0x12345678u);
    CHECK(frame_in_picture(other, kTargets) == other);
    uint64_t shown = frame_reg(kShownFbp, 3, 0, 0);
    CHECK(frame_in_picture(shown, kTargets) == shown);
}

void test_tex0_in_picture() {
    uint64_t tex0 = tex0_reg(kDrawnBlocks, 5, 0x13, 8, 7, 0x123, 5);
    uint64_t moved = tex0_in_picture(tex0, kTargets);
    CHECK(moved == tex0_reg(kShownBlocks, 5, 0x13, 8, 7, 0x123, 5));
    CHECK((moved & 0x3FFF) == kShownBlocks);
    CHECK(((moved >> 14) & 0x3F) == 5);        // TBW
    CHECK(((moved >> 20) & 0x3F) == 0x13);     // PSM
    CHECK(((moved >> 26) & 0xF) == 8);         // TW
    CHECK(((moved >> 30) & 0xF) == 7);         // TH
    CHECK(((moved >> 37) & 0x3FFF) == 0x123);  // CBP
    CHECK(((moved >> 61) & 0x7) == 5);         // CLD
    CHECK(((moved >> 34) & 0x7) == 0b101);     // TCC 1, TFX 2 (kept)

    // one block into the buffer is not its base: a texture inside the buffer, not the whole of it
    uint64_t inside = tex0_reg(kDrawnBlocks + 1, 5, 0x13, 8, 7, 0x123, 5);
    CHECK(tex0_in_picture(inside, kTargets) == inside);
    uint64_t shown = tex0_reg(kShownBlocks, 5, 0x13, 8, 7, 0x123, 5);
    CHECK(tex0_in_picture(shown, kTargets) == shown);
}

void test_tex0_reads() {
    uint64_t drawn = tex0_reg(kDrawnBlocks, 5, 0, 1, 1, 0, 0);
    uint64_t shown = tex0_reg(kShownBlocks, 5, 0, 1, 1, 0, 0);
    uint64_t inside = tex0_reg(kDrawnBlocks + 1, 5, 0, 1, 1, 0, 0);

    CHECK(tex0_reads_drawn(drawn, kTargets));
    CHECK(tex0_reads_drawn(drawn, kTargetsKept));
    CHECK(!tex0_reads_drawn(inside, kTargets));
    CHECK(!tex0_reads_drawn(shown, kTargets));

    CHECK(!tex0_reads_kept(shown, kTargets));   // the frame is not shown kept
    CHECK(tex0_reads_kept(shown, kTargetsKept));
    CHECK(!tex0_reads_kept(drawn, kTargetsKept));
    CHECK(!tex0_reads_kept(inside, kTargetsKept));
}

void test_transfer_upload() {
    TransferRegisters into_drawn{.bitbltbuf = bitbltbuf_reg(0x40, kDrawnBlocks)};
    TransferRegisters elsewhere{.bitbltbuf = bitbltbuf_reg(0x40, 0x80)};

    CHECK(transfer_in_picture(0, into_drawn, kTargets) == TransferInPicture::refused);
    CHECK(transfer_in_picture(0, into_drawn, kTargetsAsked) == TransferInPicture::refused);
    CHECK(transfer_in_picture(0, elsewhere, kTargets) == TransferInPicture::as_is);
    CHECK(transfer_in_picture(0, elsewhere, kTargetsKept) == TransferInPicture::as_is);
    CHECK(transfer_in_picture(0, elsewhere, kTargetsAsked) == TransferInPicture::asked_again);
}

void test_transfer_read_back_and_copy() {
    TransferRegisters from_drawn{.bitbltbuf = bitbltbuf_reg(kDrawnBlocks, 0x80)};
    TransferRegisters into_drawn{.bitbltbuf = bitbltbuf_reg(0x40, kDrawnBlocks)};
    TransferRegisters elsewhere{.bitbltbuf = bitbltbuf_reg(0x40, 0x80)};

    // a read-back (direction 1) is left out whatever the registers say
    CHECK(transfer_in_picture(1, from_drawn, kTargets) == TransferInPicture::left_out);
    CHECK(transfer_in_picture(1, into_drawn, kTargets) == TransferInPicture::left_out);
    CHECK(transfer_in_picture(1, elsewhere, kTargets) == TransferInPicture::left_out);

    // a local copy (direction 2): from or into the buffer drawn refused, otherwise left out
    CHECK(transfer_in_picture(2, from_drawn, kTargets) == TransferInPicture::refused);
    CHECK(transfer_in_picture(2, into_drawn, kTargets) == TransferInPicture::refused);
    CHECK(transfer_in_picture(2, elsewhere, kTargets) == TransferInPicture::left_out);
    CHECK(transfer_in_picture(2, elsewhere, kTargetsAsked) == TransferInPicture::left_out);

    // direction 3 is as it is
    CHECK(transfer_in_picture(3, from_drawn, kTargets) == TransferInPicture::as_is);
    CHECK(transfer_in_picture(3, into_drawn, kTargets) == TransferInPicture::as_is);
    CHECK(transfer_in_picture(3, elsewhere, kTargetsAsked) == TransferInPicture::as_is);
}

} // namespace

int main() {
    test_frame_in_picture();
    test_tex0_in_picture();
    test_tex0_reads();
    test_transfer_upload();
    test_transfer_read_back_and_copy();
    if (failures) {
        std::printf("pcr_frame_walk: %d failed\n", failures);
        return 1;
    }
    std::printf("pcr_frame_walk: all passed\n");
    return 0;
}
