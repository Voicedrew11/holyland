// The PC renderer's textures from their uploads (port/gs/pc/pcr_textures.cpp) against VRAM as gsn_vram.c
// has it: the same uploads written into a 4 MB VRAM in order, the same draws (random pixels) and copies,
// and a CLUT buffer loaded from that VRAM as the GS loads it (gsn.cpp's clut_load, clut_read_entries and
// clut_colours, written again here from gsn_vram.c's functions).
//
// Random uploads of every texture format to random places, overlapping and partial, rectangles that wrap
// at 2048 and at the end of VRAM, transfers cut short. After each, random textures (some mipmapped, some
// read in another format of the same layout) and CLUT loads; where the store says a texture or a CLUT is
// the uploads', every texel and colour read must be what that VRAM gives. A store that answers nothing
// would pass that, so each kind of answer must also have been met.
//
// Every call goes to a second store too that keeps no resolve's answer (every resolve walks): each answer must be
// that store's, and each watched stretch's reads too.
//
// pcr_textures_test [seed...]: the seeds to run (by default 1-6).
#include "pcr_textures.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "gsn_vram.h"

namespace {

using pcr::TextureSource;

struct Rng {
    uint64_t state;
    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    uint32_t below(uint32_t n) {
        return n ? uint32_t(next() % n) : 0;
    }
    bool one_in(uint32_t n) {
        return below(n) == 0;
    }
};

uint32_t field(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

const uint32_t kTextureFormats[] = {GSN_PSMCT32, GSN_PSMCT24, GSN_PSMCT16, GSN_PSMCT16S, GSN_PSMT8,
                                    GSN_PSMT4,   GSN_PSMT8H,  GSN_PSMT4HL, GSN_PSMT4HH};
// (the formats of the 32-bit layout: an upload in one read as another)
const uint32_t kWordFormats[] = {GSN_PSMCT32, GSN_PSMCT24, GSN_PSMT8H, GSN_PSMT4HL, GSN_PSMT4HH};

bool is_word_format(uint32_t psm) {
    for (uint32_t f : kWordFormats) {
        if (f == psm) {
            return true;
        }
    }
    return false;
}

bool is_palette(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 || psm == GSN_PSMT8H || psm == GSN_PSMT4HL || psm == GSN_PSMT4HH;
}

bool is_16bit(uint32_t psm) {
    return psm == GSN_PSMCT16 || psm == GSN_PSMCT16S;
}

// A page of a format, in pixels (the GS's manual, its memory model)
uint32_t page_width_px(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 ? 128 : 64;
}

uint32_t page_height_px(uint32_t psm) {
    return psm == GSN_PSMT4 ? 128 : psm == GSN_PSMT8 || is_16bit(psm) ? 64 : 32;
}

// The texels of one side a texture keeps: all of them with mipmaps or REGION_REPEAT, else those read
// rounded up to whole pages
uint32_t part_read(uint32_t size, uint32_t read, uint32_t page, uint32_t levels, uint32_t clamp_mode) {
    if (levels != 1 || clamp_mode == 3) {
        return size;
    }
    read = std::clamp(read, 1u, size);
    return std::min(size, (read + page - 1) / page * page);
}

uint64_t bitbltbuf_of(uint32_t bp, uint32_t bw, uint32_t psm) {
    return uint64_t(bp) << 32 | uint64_t(bw) << 48 | uint64_t(psm) << 56;
}

uint64_t tex0_of(uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t tw, uint32_t th) {
    return uint64_t(tbp) | uint64_t(tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tw) << 26 | uint64_t(th) << 30;
}

// ---- The reference: the GS's VRAM and CLUT buffer --------------------------------------------------
struct ReferenceGs {
    std::vector<uint8_t> vram = std::vector<uint8_t>(GSN_VRAM_BYTES);
    std::array<uint16_t, 512> clut{};
    std::array<uint32_t, 2> clut_cbp = {UINT32_MAX, UINT32_MAX};

    void upload(const pcr::TransferRegs &regs, const std::vector<uint8_t> &bytes) {
        GsnUpload u;
        gsn_upload_begin(&u, regs.bitbltbuf, regs.trxpos, regs.trxreg);
        gsn_upload_data(&u, vram.data(), bytes.data(), bytes.size(), nullptr, nullptr);
    }

    // A draw: random pixels over the rectangle
    void draw(const pcr::VramRect &r, Rng &rng) {
        for (uint32_t y = 0; y < r.height_px; y++) {
            for (uint32_t x = 0; x < r.width_px; x++) {
                gsn_vram_write(vram.data(), r.psm, r.bp, r.bw, (r.x + x) % 2048, (r.y + y) % 2048, uint32_t(rng.next()));
            }
        }
    }

    // A texel as the store gives it (pcr_textures.h, DecodedTexture)
    uint32_t texel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y) const {
        uint32_t v = gsn_vram_read(vram.data(), psm, bp, bw, x, y);
        if (is_palette(psm)) {
            return v;
        }
        if (is_16bit(psm)) {
            return (v & 0x1F) << 3 | ((v >> 5) & 0x1F) << 11 | ((v >> 10) & 0x1F) << 19 | ((v & 0x8000) ? 0xFFu << 24 : 0);
        }
        return psm == GSN_PSMCT24 ? v & 0xFFFFFF : v;
    }

    // gsn.cpp's clut_read_entries
    std::array<uint32_t, 256> clut_entries(uint64_t tex0, uint64_t texclut) const {
        uint32_t psm = field(tex0, 20, 6), cbp = field(tex0, 37, 14), cpsm = field(tex0, 51, 4);
        bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
        uint32_t entries = eight ? 256 : 16;
        uint32_t gpsm = cpsm == 0 ? GSN_PSMCT32 : cpsm == 2 ? GSN_PSMCT16 : GSN_PSMCT16S;
        std::array<uint32_t, 256> values{};
        if (field(tex0, 55, 1)) {
            for (uint32_t i = 0; i < entries; i++) {
                values[i] = gsn_vram_read(vram.data(), gpsm, cbp, field(texclut, 0, 6), field(texclut, 6, 6) * 16 + i,
                                          field(texclut, 12, 10));
            }
            return values;
        }
        uint32_t w = eight ? 16 : 8;
        for (uint32_t i = 0; i < entries; i++) {
            uint32_t k = eight ? ((i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)) : i;
            values[i] = gsn_vram_read(vram.data(), gpsm, cbp, 1, k % w, k / w);
        }
        return values;
    }

    // gsn.cpp's clut_load, without its memos
    void load_clut(uint64_t tex0, uint64_t texclut) {
        uint32_t psm = field(tex0, 20, 6), cbp = field(tex0, 37, 14), cpsm = field(tex0, 51, 4);
        uint32_t csa = field(tex0, 56, 5), cld = field(tex0, 61, 3);
        if (!is_palette(psm)) {
            return;
        }
        if (cld == 0 || cld > 5) {
            return;
        }
        if (cld == 2 || cld == 3) {
            clut_cbp[cld - 2] = cbp;
        }
        if (cld == 4 || cld == 5) {
            if (clut_cbp[cld - 4] == cbp) {
                return;
            }
            clut_cbp[cld - 4] = cbp;
        }
        bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
        std::array<uint32_t, 256> values = clut_entries(tex0, texclut);
        for (uint32_t i = 0; i < (eight ? 256u : 16u); i++) {
            if (cpsm == 0) {
                uint32_t at = eight ? i : (csa * 16 + i) & 0xFF;
                clut[at] = uint16_t(values[i]);
                clut[256 + at] = uint16_t(values[i] >> 16);
            } else {
                clut[eight ? i : (csa * 16 + i) & 0x1FF] = uint16_t(values[i]);
            }
        }
    }

    // gsn.cpp's clut_colours
    std::array<uint32_t, 256> clut_colours(uint64_t tex0, uint64_t texa) const {
        uint32_t psm = field(tex0, 20, 6), cpsm = field(tex0, 51, 4), csa = field(tex0, 56, 5);
        bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
        std::array<uint32_t, 256> out{};
        for (uint32_t i = 0; i < (eight ? 256u : 16u); i++) {
            if (cpsm == 0) {
                uint32_t at = eight ? i : ((csa & 15) * 16 + i) & 0xFF;
                out[i] = uint32_t(clut[at]) | (uint32_t(clut[256 + at]) << 16);
                continue;
            }
            uint32_t v = clut[eight ? i : (csa * 16 + i) & 0x1FF];
            uint32_t r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
            uint32_t a = (v & 0x8000) ? field(texa, 32, 8) : (field(texa, 15, 1) && (v & 0x7FFF) == 0) ? 0 : field(texa, 0, 8);
            out[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
        return out;
    }
};

// ---- What a run met ---------------------------------------------------------------------------------
struct Tally {
    uint64_t uploads = 0, sent_again = 0, made_again = 0, draws = 0, copies = 0, copies_checked = 0, read_backs = 0, forgets = 0;
    uint64_t answers[3] = {};
    uint64_t texels = 0, mipmapped = 0, other_format = 0, clut_compared = 0, clut_csm2 = 0, clut_16 = 0;
    uint64_t stretches = 0; // (watched stretches compared with the walking store's)
    uint64_t failures = 0;
};

bool fail(Tally &tally, const char *what) {
    if (tally.failures++ < 20) {
        fprintf(stderr, "pcr_textures_test: %s\n", what);
    }
    return false;
}

// A texture the test made: where its levels were uploaded
struct Placed {
    uint32_t psm, tw, th, levels;
    std::array<uint32_t, 7> tbp, tbw;
};

pcr::TextureStoreLimits walking(pcr::TextureStoreLimits limits) {
    limits.answers_kept = 0;
    return limits;
}

bool same_reads(const pcr::PagesRead &a, const pcr::PagesRead &b) {
    for (uint32_t p = 0; p < 513; p++) {
        if (a.pages[p] != b.pages[p] || (a.pages[p] && a.signatures[p] != b.signatures[p])) {
            return false;
        }
    }
    return true;
}

struct Run {
    Rng rng;
    pcr::TextureStore store;
    pcr::TextureStore walker; // (the same calls, no answer kept: every resolve walks)
    bool watching = false;
    ReferenceGs gs;
    Tally tally;
    std::vector<Placed> placed;
    struct Sent {
        pcr::TransferRegs regs;
        std::vector<uint8_t> bytes;
        uint64_t chunk; // (the store's, 0 if it refused it)
    };
    std::vector<Sent> recent; // (uploads to send again)

    Run(uint64_t seed, pcr::TextureStoreLimits limits)
        : rng{seed * 0x9E3779B97F4A7C15ull + 1}, store(limits), walker(walking(limits)) {}

    void upload(const pcr::TransferRegs &regs, const std::vector<uint8_t> &bytes) {
        bool taken = store.upload(regs, bytes);
        walker.upload(regs, bytes);
        gs.upload(regs, bytes);
        tally.uploads++;
        recent.push_back({regs, bytes, taken ? store.last_chunk() : 0});
        if (recent.size() > 48) {
            recent.erase(recent.begin());
        }
    }

    // One of the recent uploads sent again, the same bytes to the same place (as the game sends a texture
    // every frame): the same chunk, the newest owner again
    void upload_again() {
        if (recent.empty()) {
            return;
        }
        Sent sent = recent[rng.below(uint32_t(recent.size()))];
        tally.sent_again++;
        // (half of them made again from their chunk, as a picture between asks: the VRAM as if the bytes came)
        if (sent.chunk != 0 && rng.one_in(2) && store.upload_again(sent.chunk)) {
            if (!walker.upload_again(sent.chunk)) {
                fail(tally, "a chunk made again by the store is let go by the walking store");
            }
            gs.upload(sent.regs, sent.bytes);
            tally.made_again++;
            return;
        }
        upload(sent.regs, sent.bytes);
    }

    std::vector<uint8_t> random_bytes(size_t n) {
        std::vector<uint8_t> bytes(n);
        for (uint8_t &b : bytes) {
            b = uint8_t(rng.next() >> 24);
        }
        return bytes;
    }

    // The bytes a w x h rectangle of psm takes, now and then cut short or run long
    std::vector<uint8_t> bytes_for(uint32_t psm, uint32_t w, uint32_t h) {
        size_t n = (size_t(w) * h * gsn_psm_bits(psm) + 7) / 8;
        if (rng.one_in(8)) {
            n = rng.below(uint32_t(n) + 1);
        } else if (rng.one_in(8)) {
            n += rng.below(64);
        }
        return random_bytes(n);
    }

    // An upload anywhere near the others: random format, place, size; some wrap at 2048 or 4 MB
    void random_upload() {
        uint32_t psm = kTextureFormats[rng.below(9)];
        uint32_t bp = rng.one_in(16) ? 16384 - 1 - rng.below(64) : rng.below(2048);
        uint32_t bw = 1 + rng.below(8);
        uint32_t x = rng.one_in(8) ? 2048 - 1 - rng.below(32) : rng.below(96);
        uint32_t y = rng.one_in(8) ? 2048 - 1 - rng.below(32) : rng.below(96);
        uint32_t w = 1 + rng.below(rng.one_in(2) ? 24 : 160), h = 1 + rng.below(rng.one_in(2) ? 24 : 160);
        pcr::TransferRegs regs{bitbltbuf_of(bp, bw, psm), uint64_t(x) << 32 | uint64_t(y) << 48,
                               uint64_t(w) | uint64_t(h) << 32};
        upload(regs, bytes_for(psm, w, h));
    }

    // A texture's levels uploaded whole, each at its own place (levels > 1: mipmapped), to be resolved
    void place_texture() {
        Placed p{};
        p.psm = kTextureFormats[rng.below(9)];
        p.tw = rng.below(8);
        p.th = rng.below(8);
        p.levels = rng.one_in(3) ? 2 + rng.below(4) : 1;
        uint32_t bits = gsn_psm_bits(p.psm);
        for (uint32_t l = 0; l < p.levels; l++) {
            uint32_t w = std::max(1u, (1u << p.tw) >> l), h = std::max(1u, (1u << p.th) >> l);
            p.tbw[l] = std::max(1u, (w + 63) / 64) + rng.below(2);
            if ((p.psm == GSN_PSMT8 || p.psm == GSN_PSMT4) && (p.tbw[l] & 1)) {
                p.tbw[l]++;
            }
            p.tbp[l] = rng.below(2048);
            pcr::TransferRegs regs{bitbltbuf_of(p.tbp[l], p.tbw[l], p.psm), 0, uint64_t(w) | uint64_t(h) << 32};
            upload(regs, random_bytes((size_t(w) * h * bits + 7) / 8));
        }
        placed.push_back(p);
        // just uploaded, all of it is the uploads'
        pcr::TextureRegs regs;
        regs.tex0 = tex0_of(p.tbp[0], p.tbw[0], p.psm, p.tw, p.th);
        regs.tex1 = uint64_t(p.levels - 1) << 2 | uint64_t(4) << 6;
        for (uint32_t l = 1; l < p.levels; l++) {
            (l <= 3 ? regs.miptbp1 : regs.miptbp2) |= (uint64_t(p.tbp[l]) | uint64_t(p.tbw[l]) << 14) << ((l - 1) % 3 * 20);
        }
        if (check(regs, 1u << p.tw, 1u << p.th) != TextureSource::FromUploads) {
            fail(tally, "a texture just uploaded whole is not the uploads'");
        }
        if (placed.size() > 64) {
            placed.erase(placed.begin());
        }
    }

    // A draw into a surface: random pixels in the reference, drawn in the store
    void random_draw() {
        static const uint32_t formats[] = {GSN_PSMCT32, GSN_PSMCT24, GSN_PSMCT16, GSN_PSMCT16S, GSN_PSMT8};
        pcr::VramRect r{rng.below(2048), 1 + rng.below(4), formats[rng.below(5)], rng.below(64), rng.below(64),
                        1 + rng.below(64), 1 + rng.below(32)};
        gs.draw(r, rng);
        store.drawn(r);
        walker.drawn(r);
        tally.draws++;
    }

    // A placed texture's rows moved within it or to a place of their own, in its own format (as the game scrolls a
    // texture: rows copied round), then the texture asked for, which is the copy's then
    void scroll_copy() {
        if (placed.empty()) {
            return;
        }
        const Placed &p = placed[rng.below(uint32_t(placed.size()))];
        uint32_t w = 1u << p.tw, h = 1u << p.th, rows = 1 + rng.below(h), from = rng.below(h - rows + 1);
        bool elsewhere = rng.one_in(2);
        uint32_t dbp = elsewhere ? rng.below(2048) * 8 : p.tbp[0];
        uint32_t to = elsewhere ? 0 : rng.below(h - rows + 1);
        pcr::TransferRegs regs{bitbltbuf_of(dbp, p.tbw[0], p.psm) | (bitbltbuf_of(p.tbp[0], p.tbw[0], p.psm) >> 32),
                               uint64_t(from) << 16 | uint64_t(to) << 48, uint64_t(w) | uint64_t(rows) << 32};
        gsn_vram_copy(gs.vram.data(), regs.bitbltbuf, regs.trxpos, regs.trxreg);
        store.copied(regs);
        walker.copied(regs);
        tally.copies++;
        pcr::TextureRegs t;
        t.tex0 = tex0_of(dbp, p.tbw[0], p.psm, p.tw, p.th);
        if (check(t, w, h) == TextureSource::FromUploads) {
            tally.copies_checked++;
        }
    }

    // A placed texture's part read back (local to host), in its own format or as 32-bit words, against the GS's
    void read_back_round() {
        if (placed.empty()) {
            return;
        }
        const Placed &p = placed[rng.below(uint32_t(placed.size()))];
        uint32_t psm = rng.one_in(3) ? GSN_PSMCT32 : p.psm;
        uint32_t w = 1 + rng.below(1u << p.tw), h = 1 + rng.below(1u << p.th);
        pcr::TransferRegs regs{bitbltbuf_of(p.tbp[0], p.tbw[0], psm) >> 32,
                               uint64_t(rng.below(8)) | uint64_t(rng.below(8)) << 16, uint64_t(w) | uint64_t(h) << 32};
        std::vector<uint8_t> mine, walked;
        bool had = walker.read_back(regs, walked);
        if (store.read_back(regs, mine) != had) {
            fail(tally, "a read-back is not the walking store's");
        }
        if (!had) {
            return;
        }
        std::vector<uint8_t> want(mine.size());
        gsn_vram_read_image(gs.vram.data(), regs.bitbltbuf, regs.trxpos, regs.trxreg, want.data(), want.size() / 16);
        tally.read_backs++;
        if (mine != want) {
            fail(tally, "a read-back is not the GS's");
        }
    }

    void random_copy() {
        if (rng.one_in(3)) {
            read_back_round();
            return;
        }
        if (rng.one_in(2)) {
            scroll_copy();
            return;
        }
        uint32_t psm = kWordFormats[rng.below(2)];
        pcr::TransferRegs regs{uint64_t(rng.below(2048)) | uint64_t(1 + rng.below(4)) << 16 | uint64_t(psm) << 24 |
                                   bitbltbuf_of(rng.below(2048), 1 + rng.below(4), psm),
                               uint64_t(rng.below(64)) | uint64_t(rng.below(64)) << 16 | uint64_t(rng.below(64)) << 32 |
                                   uint64_t(rng.below(64)) << 48,
                               uint64_t(1 + rng.below(48)) | uint64_t(1 + rng.below(48)) << 32};
        gsn_vram_copy(gs.vram.data(), regs.bitbltbuf, regs.trxpos, regs.trxreg);
        store.copied(regs);
        walker.copied(regs);
        tally.copies++;
    }

    // The texture's answer checked: every texel of every level of the part read against the reference
    // The answer of both stores: the same source, key and missing block
    pcr::TextureAnswer resolve_both(const pcr::TextureRegs &regs, uint32_t cols_read, uint32_t rows_read) {
        pcr::TextureAnswer a = store.resolve(regs, cols_read, rows_read);
        pcr::TextureAnswer w = walker.resolve(regs, cols_read, rows_read);
        if (a.source != w.source || !(a.key == w.key) || a.missing.level != w.missing.level ||
            a.missing.block != w.missing.block) {
            fail(tally, "a resolve's answer is not the walking store's");
        }
        return a;
    }

    TextureSource check(const pcr::TextureRegs &regs, uint32_t cols_read, uint32_t rows_read) {
        pcr::TextureAnswer a = resolve_both(regs, cols_read, rows_read);
        tally.answers[int(a.source)]++;
        if (a.source != TextureSource::FromUploads) {
            return a.source;
        }
        uint32_t psm = field(regs.tex0, 20, 6), tw = std::min(field(regs.tex0, 26, 4), 10u);
        uint32_t th = std::min(field(regs.tex0, 30, 4), 10u);
        const pcr::DecodedTexture *t = a.texture.get();
        if (!t || t->width_px != 1u << tw || t->height_px != 1u << th || t->levels != a.key.levels ||
            (t->kind == pcr::TexelKind::Index8) != is_palette(psm)) {
            fail(tally, "a texture's shape is not its TEX0's");
            return a.source;
        }
        if (t->read_cols != part_read(t->width_px, cols_read, page_width_px(psm), t->levels, field(regs.clamp, 0, 2)) ||
            t->read_rows != part_read(t->height_px, rows_read, page_height_px(psm), t->levels, field(regs.clamp, 2, 2))) {
            fail(tally, "a texture's part read is not the texels asked for in whole pages");
            return a.source;
        }
        for (uint32_t l = 0; l < t->levels; l++) {
            uint32_t tbp = l == 0 ? field(regs.tex0, 0, 14) : field(l <= 3 ? regs.miptbp1 : regs.miptbp2, int((l - 1) % 3) * 20, 14);
            uint32_t tbw = l == 0 ? field(regs.tex0, 14, 6) : field(l <= 3 ? regs.miptbp1 : regs.miptbp2, int((l - 1) % 3) * 20 + 14, 6);
            uint32_t cols = std::max(1u, t->read_cols >> l), rows = std::max(1u, t->read_rows >> l);
            for (uint32_t y = 0; y < rows; y++) {
                for (uint32_t x = 0; x < cols; x++) {
                    size_t at = t->level_start[l] + size_t(y) * t->level_width(l) + x;
                    uint32_t got = t->kind == pcr::TexelKind::Index8 ? t->indices[at] : t->rgba[at];
                    uint32_t want = gs.texel(psm, tbp, tbw, x, y);
                    tally.texels++;
                    if (got != want) {
                        char what[200];
                        snprintf(what, sizeof(what), "psm %02x tbp %x tbw %u level %u (%u, %u): store %08x, VRAM %08x", psm,
                                 tbp, tbw, l, x, y, got, want);
                        fail(tally, what);
                        return a.source;
                    }
                }
            }
        }
        // asked again, the same key and the same texels (kept or decoded again)
        pcr::TextureAnswer again = resolve_both(regs, cols_read, rows_read);
        if (again.source != TextureSource::FromUploads || !(again.key == a.key) ||
            again.texture->rgba != t->rgba || again.texture->indices != t->indices) {
            fail(tally, "a texture asked for again is not the same");
        }
        return a.source;
    }

    // One of the placed textures, sometimes read in another format of its layout, sometimes with the levels
    void resolve_placed() {
        if (placed.empty()) {
            return;
        }
        const Placed &p = placed[rng.below(uint32_t(placed.size()))];
        uint32_t psm = p.psm;
        if (is_word_format(psm) && rng.one_in(2)) {
            psm = kWordFormats[rng.below(5)];
            tally.other_format += psm != p.psm;
        }
        uint32_t tw = p.tw - (p.tw && rng.one_in(4) ? 1 : 0), th = p.th;
        pcr::TextureRegs regs;
        regs.tex0 = tex0_of(p.tbp[0], p.tbw[0], psm, tw, th);
        if (p.levels > 1 && !rng.one_in(4)) {
            regs.tex1 = uint64_t(p.levels - 1) << 2 | uint64_t(2 + rng.below(4)) << 6;
            for (uint32_t l = 1; l < p.levels; l++) {
                uint64_t &m = l <= 3 ? regs.miptbp1 : regs.miptbp2;
                m |= (uint64_t(p.tbp[l]) | uint64_t(p.tbw[l]) << 14) << ((l - 1) % 3 * 20);
            }
            tally.mipmapped++;
        }
        regs.clamp = rng.below(16);
        check(regs, 1 + rng.below(1u << tw), 1 + rng.below(1u << th));
    }

    // A texture anywhere: mostly not the uploads'
    void resolve_anywhere() {
        pcr::TextureRegs regs;
        regs.tex0 = tex0_of(rng.below(2048), 1 + rng.below(4), kTextureFormats[rng.below(9)], rng.below(7), rng.below(7));
        regs.clamp = rng.below(16);
        check(regs, 1 + rng.below(64), 1 + rng.below(64));
    }

    // A CLUT uploaded where a load will read it, sometimes; then a load (CLD 0-5) and the colours compared
    void clut_round() {
        uint32_t psm = kTextureFormats[4 + rng.below(5)];
        bool eight = psm == GSN_PSMT8 || psm == GSN_PSMT8H;
        static const uint32_t cpsms[] = {0, 2, 10};
        uint32_t cpsm = cpsms[rng.below(3)], csm = rng.below(2), csa = rng.below(32), cld = rng.below(6);
        uint32_t gpsm = cpsm == 0 ? GSN_PSMCT32 : cpsm == 2 ? GSN_PSMCT16 : GSN_PSMCT16S;
        uint32_t cbp = rng.below(12) * 40; // (a few places: CLD 4 and 5 meet the CBP they have)
        uint64_t texclut = uint64_t(1 + rng.below(4)) | uint64_t(rng.below(8)) << 6 | uint64_t(rng.below(32)) << 12;
        if (!rng.one_in(4)) {
            // the CLUT's upload: CSM1's rectangle, or TEXCLUT's line
            uint32_t w = csm ? (eight ? 256 : 16) : eight ? 16 : 8, h = csm ? 1 : eight ? 16 : 2;
            uint32_t bw = csm ? field(texclut, 0, 6) : 1;
            uint32_t x = csm ? field(texclut, 6, 6) * 16 : 0, y = csm ? field(texclut, 12, 10) : 0;
            pcr::TransferRegs regs{bitbltbuf_of(cbp, bw, gpsm), uint64_t(x) << 32 | uint64_t(y) << 48,
                                   uint64_t(w) | uint64_t(h) << 32};
            upload(regs, random_bytes(size_t(w) * h * gsn_psm_bits(gpsm) / 8));
        }
        uint64_t tex0 = tex0_of(0, 1, psm, 4, 4) | uint64_t(cbp) << 37 | uint64_t(cpsm) << 51 | uint64_t(csm) << 55 |
                        uint64_t(csa) << 56 | uint64_t(cld) << 61;
        if (store.load_clut(tex0, texclut) != walker.load_clut(tex0, texclut)) {
            fail(tally, "a CLUT load is not the walking store's");
        }
        gs.load_clut(tex0, texclut);
        uint64_t texa = uint64_t(rng.below(256)) | uint64_t(rng.below(2)) << 15 | uint64_t(rng.below(256)) << 32;
        // the colours of this TEX0, and of another CSA's over the same buffer
        for (uint64_t read : {tex0, (tex0 & ~(uint64_t(31) << 56)) | uint64_t(rng.below(32)) << 56}) {
            pcr::ClutColours mine = store.clut_colours(read, texa);
            pcr::ClutColours walked = walker.clut_colours(read, texa);
            if (mine.rgba != walked.rgba || mine.from_uploads != walked.from_uploads) {
                fail(tally, "a CLUT's colours are not the walking store's");
            }
            if (!mine.from_uploads) {
                continue;
            }
            tally.clut_compared++;
            tally.clut_csm2 += csm;
            tally.clut_16 += !eight;
            if (mine.rgba != gs.clut_colours(read, texa)) {
                fail(tally, "a CLUT's colours are not the GS's");
            }
        }
    }

    // The edges: transfers off VRAM, empty, of no format; textures and CLUTs past the ends
    void edges() {
        const pcr::TransferRegs transfers[] = {
            {bitbltbuf_of(0, 1, GSN_PSMCT32), 0, 0},                                    // no pixels
            {bitbltbuf_of(0, 1, GSN_PSMCT32), 0, uint64_t(16)},                          // no rows
            {bitbltbuf_of(0, 1, 0x05), 0, uint64_t(8) | uint64_t(8) << 32},              // no format the GS has
            {bitbltbuf_of(16383, 63, GSN_PSMCT32), uint64_t(2047) << 32 | uint64_t(2047) << 48,
             uint64_t(4095) | uint64_t(4095) << 32},                                     // all off the end, cut short
            {bitbltbuf_of(16380, 2, GSN_PSMT4), uint64_t(2040) << 32 | uint64_t(5) << 48,
             uint64_t(33) | uint64_t(7) << 32},                                          // odd rows of 4 bits, wrapping
            {bitbltbuf_of(16376, 1, GSN_PSMCT24), uint64_t(2044) << 32, uint64_t(11) | uint64_t(9) << 32},
            {bitbltbuf_of(16383, 1, GSN_PSMT4HH), uint64_t(2047) << 48, uint64_t(64) | uint64_t(64) << 32},
        };
        const size_t sizes[] = {64, 64, 256, 4096, 116, 296, 2048};
        for (size_t i = 0; i < std::size(transfers); i++) {
            std::vector<uint8_t> bytes = random_bytes(sizes[i]);
            upload(transfers[i], bytes);
            upload(transfers[i], {}); // (no bytes)
        }
        store.drawn({0x7FFFFFFF, 0xFFFF, 0x3F, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 3});
        walker.drawn({0x7FFFFFFF, 0xFFFF, 0x3F, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 3});
        store.copied({~uint64_t(0), ~uint64_t(0), ~uint64_t(0)});
        walker.copied({~uint64_t(0), ~uint64_t(0), ~uint64_t(0)});
        pcr::TextureRegs regs;
        regs.tex0 = ~uint64_t(0) >> 3; // (TW and TH 15, psm 0x3F: no format)
        check(regs, 0, 0);
        regs.tex0 = tex0_of(16383, 63, GSN_PSMT4, 15, 15);
        regs.tex1 = uint64_t(7) << 2 | uint64_t(4) << 6;
        regs.miptbp1 = regs.miptbp2 = ~uint64_t(0);
        regs.clamp = 0xF;
        check(regs, 1u << 31, 1u << 31);
        regs.tex0 = tex0_of(16380, 2, GSN_PSMT4, 3, 3); // (over the 4-bit wrap above)
        regs.tex1 = 0;
        check(regs, 8, 8);
        uint64_t clut0 = tex0_of(0, 1, GSN_PSMT8, 4, 4) | uint64_t(16383) << 37 | uint64_t(1) << 55 | uint64_t(1) << 61;
        store.load_clut(clut0, 0x3FFFFF);
        walker.load_clut(clut0, 0x3FFFFF);
        gs.load_clut(clut0, 0x3FFFFF);
        store.clut_colours(~uint64_t(0), ~uint64_t(0));
        walker.clut_colours(~uint64_t(0), ~uint64_t(0));
    }

    // A stretch's reads watched in both stores, from now or until now: the same pages with the same signatures
    void watch_round() {
        if (!watching) {
            store.watch_reads();
            walker.watch_reads();
            watching = true;
            return;
        }
        if (!same_reads(store.reads_watched(), walker.reads_watched())) {
            fail(tally, "a stretch's reads are not the walking store's");
        }
        watching = false;
        tally.stretches++;
    }

    void step() {
        if (rng.one_in(40)) {
            watch_round();
        }
        uint32_t what = rng.below(100);
        if (what < 22) {
            random_upload();
        } else if (what < 30) {
            upload_again();
        } else if (what < 45) {
            place_texture();
        } else if (what < 75) {
            resolve_placed();
        } else if (what < 82) {
            resolve_anywhere();
        } else if (what < 92) {
            clut_round();
        } else if (what < 97) {
            random_draw();
        } else if (what < 99) {
            random_copy();
        } else if (rng.one_in(10)) {
            store.forget();
            walker.forget();
            gs.clut_cbp = {UINT32_MAX, UINT32_MAX}; // (a state loaded: CLD 4 and 5 load again, as the store does)
            tally.forgets++;
        }
    }
};

// Uploads only: the store's VRAM image is the reference's whole; restored into a new store, its textures are too
bool vram_round_trip(uint64_t seed) {
    Run run(seed, pcr::TextureStoreLimits{});
    for (int i = 0; i < 400; i++) {
        run.random_upload();
    }
    std::vector<uint8_t> image = run.store.vram_image();
    if (image != run.gs.vram) {
        size_t at = 0;
        while (at < image.size() && image[at] == run.gs.vram[at]) {
            at++;
        }
        printf("pcr_textures_test: seed %llu: the VRAM image differs from the GS's at byte %zu\n",
               (unsigned long long)seed, at);
        return false;
    }
    pcr::TextureStore restored, restored_walker(walking({}));
    restored.restore_vram(image);
    restored_walker.restore_vram(image);
    std::swap(run.store, restored);
    std::swap(run.walker, restored_walker);
    uint64_t failures = run.tally.failures;
    for (int i = 0; i < 300; i++) {
        run.resolve_placed();
    }
    if (run.tally.failures != failures) {
        printf("pcr_textures_test: seed %llu: a texture of the restored VRAM is not the GS's\n", (unsigned long long)seed);
        return false;
    }
    return true;
}

bool run_seed(uint64_t seed) {
    // (every other seed with small limits: chunks owning nothing and decodes let go often)
    pcr::TextureStoreLimits limits;
    if (seed % 2 == 0) {
        limits.decoded_bytes = 64 << 10;
        limits.unowned_chunk_bytes = 16 << 10;
    }
    Run run(seed, limits);
    run.edges();
    for (int i = 0; i < 6000; i++) {
        run.step();
    }
    run.edges();
    const Tally &t = run.tally;
    pcr::TextureStoreCounts c = run.store.counts();
    printf("seed %llu: %llu uploads (%llu sent again, %llu from their chunk), %llu draws, %llu copies (%llu read back as uploads), %llu read-backs, %llu forgets; textures %llu from uploads, %llu drawn, "
           "%llu not uploaded (%llu mipmapped, %llu in another format); %llu texels compared; CLUTs %llu compared (%llu "
           "CSM2, %llu of 16); store: %llu chunks kept, %llu decodes, %llu hits, %llu answered kept, %llu CLUT decodes, %llu refused; "
           "%llu stretches compared\n",
           (unsigned long long)seed, (unsigned long long)t.uploads, (unsigned long long)t.sent_again, (unsigned long long)t.made_again,
           (unsigned long long)t.draws,
           (unsigned long long)t.copies, (unsigned long long)t.copies_checked, (unsigned long long)t.read_backs, (unsigned long long)t.forgets, (unsigned long long)t.answers[0],
           (unsigned long long)t.answers[1], (unsigned long long)t.answers[2], (unsigned long long)t.mipmapped,
           (unsigned long long)t.other_format, (unsigned long long)t.texels, (unsigned long long)t.clut_compared,
           (unsigned long long)t.clut_csm2, (unsigned long long)t.clut_16, (unsigned long long)c.chunks_kept,
           (unsigned long long)c.decodes, (unsigned long long)c.decode_hits, (unsigned long long)c.resolves_kept,
           (unsigned long long)c.clut_decodes, (unsigned long long)c.uploads_refused, (unsigned long long)t.stretches);
    Tally check = t;
    bool met_all = t.answers[0] > 100 && c.resolves_kept > 100 && t.stretches > 20 && t.answers[1] > 0 && t.answers[2] > 0 && t.mipmapped > 0 && t.other_format > 0 &&
                   t.clut_compared > 50 && t.sent_again > 0 && t.clut_csm2 > 0 && t.clut_16 > 0 && c.uploads_refused > 0;
    if (!met_all) {
        fail(check, "a kind of answer was not met: the test tests too little");
    }
    return check.failures == 0;
}

// colour_texels: each format's TEXA and the CLUT, against the shader's rules written out by hand
bool colour_texels_rules() {
    bool ok = true;
    auto expect = [&](const char *what, uint32_t got, uint32_t want) {
        if (got != want) {
            printf("pcr_textures_test: colour_texels, %s: %08x, not %08x\n", what, got, want);
            ok = false;
        }
    };
    pcr::DecodedTexture t;
    t.kind = pcr::TexelKind::Rgba8;
    t.width_px = 4;
    t.height_px = 1;
    t.levels = 1;
    t.rgba = {0x00000000u, 0x00102030u, 0xFF000000u, 0xFF102030u};
    pcr::ClutColours none;
    const uint64_t ta0 = 0x40, ta1 = uint64_t(0x90) << 32, aem = 1u << 15;
    std::vector<uint32_t> c32 = pcr::colour_texels(t, 0x00, none, ta0 | ta1 | aem);
    expect("CT32 as it is", c32[3], 0xFF102030u);
    std::vector<uint32_t> c24 = pcr::colour_texels(t, 0x01, none, ta0 | ta1);
    expect("CT24 black, no AEM", c24[0], 0x40000000u);
    expect("CT24 colour", c24[3], 0x40102030u);
    std::vector<uint32_t> c24a = pcr::colour_texels(t, 0x31, none, ta0 | ta1 | aem);
    expect("Z24 black with AEM", c24a[0], 0x00000000u);
    expect("Z24 colour with AEM", c24a[1], 0x40102030u);
    std::vector<uint32_t> c16 = pcr::colour_texels(t, 0x02, none, ta0 | ta1 | aem);
    expect("CT16 black, bit 15 clear, AEM", c16[0], 0x00000000u);
    expect("CT16 colour, bit 15 clear", c16[1], 0x40102030u);
    expect("CT16 black, bit 15 set, AEM", c16[2], 0x90000000u);
    expect("CT16S colour, bit 15 set", pcr::colour_texels(t, 0x0A, none, ta0 | ta1)[3], 0x90102030u);
    pcr::DecodedTexture p;
    p.kind = pcr::TexelKind::Index8;
    p.width_px = 2;
    p.height_px = 2;
    p.levels = 2;
    p.level_start = {0, 4};
    p.indices = {0, 1, 255, 7, 3};
    pcr::ClutColours clut;
    for (uint32_t i = 0; i < 256; i++) {
        clut.rgba[i] = i * 0x01010101u ^ 0x80000000u;
    }
    std::vector<uint32_t> pal = pcr::colour_texels(p, 0x13, clut, ta0);
    if (pal.size() != 5) {
        printf("pcr_textures_test: colour_texels, palette: %zu texels, not 5\n", pal.size());
        return false;
    }
    expect("palette 255", pal[2], clut.rgba[255]);
    expect("palette, level 1", pal[4], clut.rgba[3]);
    return ok;
}

// pages_of against every pixel's address as gsn_vram.c has it: random rectangles of every format (the Z formats too),
// bases on a page's start and not, widths odd and 0, rectangles that wrap at 2048 and at the end of VRAM
bool pages_of_rules(uint64_t seed) {
    const uint32_t formats[] = {GSN_PSMCT32, GSN_PSMCT24, GSN_PSMCT16, GSN_PSMCT16S, GSN_PSMT8,  GSN_PSMT4,
                                GSN_PSMT8H,  GSN_PSMT4HL, GSN_PSMT4HH, GSN_PSMZ32,   GSN_PSMZ24, GSN_PSMZ16,
                                GSN_PSMZ16S};
    Rng rng{seed * 0x9E3779B97F4A7C15ull + 7};
    for (int round = 0; round < 3000; round++) {
        pcr::VramRect r;
        r.psm = formats[rng.below(std::size(formats))];
        r.bp = rng.one_in(3) ? rng.below(0x4000) : rng.below(512) * 32;
        r.bw = rng.one_in(8) ? rng.below(64) : 1 + rng.below(16);
        r.x = rng.one_in(4) ? 2048 - rng.below(200) : rng.below(1024);
        r.y = rng.one_in(4) ? 2048 - rng.below(200) : rng.below(1024);
        r.width_px = rng.one_in(10) ? 0 : 1 + rng.below(rng.one_in(8) ? 2048 : 300);
        r.height_px = 1 + rng.below(rng.one_in(8) ? 40 : 300);
        std::bitset<512> want;
        for (uint32_t y = 0; y < r.height_px; y++) {
            for (uint32_t x = 0; x < r.width_px; x++) {
                uint32_t at = gsn_vram_offset(r.psm, r.bp, r.bw, (r.x + x) % 2048, (r.y + y) % 2048, nullptr);
                want.set(at / 8192 % 512);
            }
        }
        std::bitset<512> got = pcr::pages_of(r);
        if (got != want) {
            printf("pcr_textures_test: pages_of, seed %llu round %d: psm 0x%x bp 0x%x bw %u (%u, %u) %ux%u: %zu pages, "
                   "not %zu\n",
                   (unsigned long long)seed, round, r.psm, r.bp, r.bw, r.x, r.y, r.width_px, r.height_px, got.count(),
                   want.count());
            return false;
        }
    }
    return true;
}

// ---- The signatures and the watch of a stretch's reads ------------------------------------------
bool expect_true(bool held, const char *what) {
    if (!held) {
        printf("pcr_textures_test: %s\n", what);
    }
    return held;
}

std::vector<uint8_t> bytes_from(Rng &rng, size_t n) {
    std::vector<uint8_t> bytes(n);
    for (uint8_t &b : bytes) {
        b = uint8_t(rng.next() >> 24);
    }
    return bytes;
}

// A w x h rectangle at bp (bw 1 for a page's worth of 64 x 32 32-bit pixels) as an upload's registers
pcr::TransferRegs upload_regs(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t w, uint32_t h) {
    return {bitbltbuf_of(bp, bw, psm), 0, uint64_t(w) | uint64_t(h) << 32};
}

// Every page's signature (512 the CLUT buffer)
std::vector<uint64_t> signatures_of(const pcr::TextureStore &store) {
    std::vector<uint64_t> signatures;
    for (uint32_t p = 0; p <= 512; p++) {
        signatures.push_back(store.page_signature(p));
    }
    return signatures;
}

// The pages (512 the CLUT buffer) whose signatures differ
std::bitset<513> pages_changed(const std::vector<uint64_t> &before, const std::vector<uint64_t> &after) {
    std::bitset<513> changed;
    for (size_t p = 0; p < before.size(); p++) {
        if (before[p] != after[p]) {
            changed.set(p);
        }
    }
    return changed;
}

std::bitset<513> pages_of_list(std::initializer_list<uint32_t> list) {
    std::bitset<513> pages;
    for (uint32_t p : list) {
        pages.set(p);
    }
    return pages;
}

// An upload changes the signatures of the pages it covers only; upload again of the current owner, and draws over
// it, leave a page as it was; a different upload changes it; forget returns every page to the empty store's answer
bool signatures_of_uploads() {
    Rng rng{21};
    pcr::TextureStore store;
    const uint32_t psm = GSN_PSMCT32;
    std::vector<uint64_t> start = signatures_of(store);
    store.upload(upload_regs(5 * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); // page 5
    std::vector<uint64_t> uploaded = signatures_of(store);
    bool ok = expect_true(pages_changed(start, uploaded) == pages_of_list({5}), "an upload did not change exactly its pages");
    pcr::VramRect page5{5 * 32, 1, psm, 0, 0, 64, 32};
    ok = expect_true(pcr::pages_of(page5) == std::bitset<512>().set(5), "the test's rectangle is not page 5") && ok;

    store.upload_again(store.last_chunk());
    ok = expect_true(pages_changed(uploaded, signatures_of(store)).none(), "an upload again of the current owner changed a signature") && ok;

    store.drawn(page5);
    uint64_t drawn_first = store.page_signature(5);
    ok = expect_true(drawn_first != uploaded[5], "a draw over an owner did not change its page's signature") && ok;
    store.upload_again(store.last_chunk());
    ok = expect_true(store.page_signature(5) != drawn_first, "an upload again over a drawn owner did not clear the draw") && ok;
    store.drawn(page5);
    ok = expect_true(store.page_signature(5) == drawn_first, "a draw over the owner again changed its page's signature") && ok;

    store.upload(upload_regs(5 * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); // a different upload
    ok = expect_true(store.page_signature(5) != drawn_first, "a different upload did not change its page's signature") && ok;

    pcr::TextureStore empty;
    store.forget();
    bool as_empty = true;
    for (uint32_t p = 0; p <= 512; p++) {
        as_empty = as_empty && store.page_signature(p) == empty.page_signature(p);
    }
    return expect_true(as_empty, "forget did not return every page to the empty store's answer, the CLUT buffer included") && ok;
}

// A stretch's reads: a resolve over two uploaded pages reads exactly those, with their signatures then; an upload
// elsewhere leaves them unchanged, an upload over one of them does not
bool watched_reads() {
    Rng rng{22};
    pcr::TextureStore store;
    const uint32_t psm = GSN_PSMCT32;
    store.upload(upload_regs(40 * 32, 1, psm, 64, 64), bytes_from(rng, 16384)); // pages 40 and 41
    store.upload(upload_regs(300 * 32, 1, psm, 64, 32), bytes_from(rng, 8192));
    std::vector<uint64_t> before = signatures_of(store);
    store.watch_reads();
    pcr::TextureRegs regs;
    regs.tex0 = tex0_of(40 * 32, 1, psm, 6, 6);
    pcr::TextureAnswer answer = store.resolve(regs, 64, 64);
    pcr::PagesRead read = store.reads_watched();
    bool ok = expect_true(answer.source == TextureSource::FromUploads, "a texture over two uploaded pages is not the uploads'");
    ok = expect_true(read.pages == pages_of_list({40, 41}), "the reads watched are not the texture's two pages") && ok;
    ok = expect_true(read.signatures[40] == before[40] && read.signatures[41] == before[41], "the reads' signatures are not then") && ok;

    store.upload(upload_regs(500 * 32, 1, psm, 64, 32), bytes_from(rng, 8192));
    ok = expect_true(store.unchanged(read), "an upload elsewhere changed the reads") && ok;
    store.upload(upload_regs(41 * 32, 1, psm, 64, 32), bytes_from(rng, 8192));
    ok = expect_true(!store.unchanged(read), "an upload over a page read left the reads unchanged") && ok;
    return ok;
}

// Reads before writes: a page written in the stretch and then read is no read of it; a page read and then written
// is one
bool read_before_write() {
    Rng rng{23};
    pcr::TextureStore store;
    const uint32_t psm = GSN_PSMCT32;
    store.upload(upload_regs(60 * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); // page 60, before the stretch
    uint64_t page60_then = store.page_signature(60);
    store.watch_reads();
    store.upload(upload_regs(50 * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); // page 50, written first
    pcr::TextureRegs over50;
    over50.tex0 = tex0_of(50 * 32, 1, psm, 6, 5);
    bool ok = expect_true(store.resolve(over50, 64, 32).source == TextureSource::FromUploads, "page 50 is not the uploads'");
    pcr::TextureRegs over60;
    over60.tex0 = tex0_of(60 * 32, 1, psm, 6, 5);
    store.resolve(over60, 64, 32);
    store.upload(upload_regs(60 * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); // page 60, read first, then written
    pcr::PagesRead read = store.reads_watched();
    ok = expect_true(!read.pages[50], "a page written in the stretch and then read is among its reads") && ok;
    ok = expect_true(read.pages[60] && read.signatures[60] == page60_then, "a page read and then written is not among the reads") && ok;
    ok = expect_true(!store.unchanged(read), "the write over page 60 left the reads unchanged") && ok;
    return ok;
}

// The CLUT buffer (page 512): a load that changes the mirror changes its signature, a reload that changes nothing does
// not; the colours read inside a stretch are among its reads
bool clut_signatures() {
    Rng rng{24};
    pcr::TextureStore store;
    const uint32_t cbp = 100; // (a CT32 16 x 16 CLUT at block 100)
    const uint64_t tex0 = tex0_of(0, 1, GSN_PSMT8, 4, 4) | uint64_t(cbp) << 37 | uint64_t(1) << 61; // (CLD 1)
    store.upload(upload_regs(cbp, 1, GSN_PSMCT32, 16, 16), bytes_from(rng, 1024));
    uint64_t start = store.page_signature(512);
    store.load_clut(tex0, 0);
    uint64_t loaded = store.page_signature(512);
    bool ok = expect_true(loaded != start, "a load that changed the CLUT did not change the CLUT buffer's signature");
    store.load_clut(tex0, 0);
    ok = expect_true(store.page_signature(512) == loaded, "a reload that changed nothing changed the CLUT buffer's signature") && ok;
    store.upload(upload_regs(cbp, 1, GSN_PSMCT32, 16, 16), bytes_from(rng, 1024));
    store.load_clut(tex0, 0);
    ok = expect_true(store.page_signature(512) != loaded, "a load of different entries did not change the CLUT buffer's signature") && ok;
    store.watch_reads();
    store.clut_colours(tex0, 0);
    pcr::PagesRead read = store.reads_watched();
    return expect_true(read.pages[512], "colours read in a stretch are not among its reads") && ok;
}

// A stretch that re-uploads the same chunk over owned blocks, then reads them, is unchanged right after; and the walk
// that reads a page drawn over by the last walk, re-uploads its owner and draws over it again, is unchanged too
bool reupload_in_stretch() {
    Rng rng{26};
    pcr::TextureStore store;
    const uint32_t psm = GSN_PSMCT32, page = 80;
    store.upload(upload_regs(page * 32, 1, psm, 64, 32), bytes_from(rng, 8192));
    uint64_t chunk = store.last_chunk();
    store.watch_reads();
    store.upload_again(chunk);
    pcr::TextureRegs regs;
    regs.tex0 = tex0_of(page * 32, 1, psm, 6, 5);
    bool ok = expect_true(store.resolve(regs, 64, 32).source == TextureSource::FromUploads, "the re-uploaded page is not the uploads'");
    pcr::PagesRead read = store.reads_watched();
    ok = expect_true(read.pages[page] && store.unchanged(read), "a re-upload of the same chunk over owned blocks changed the reads") && ok;

    store.drawn(pcr::VramRect{page * 32, 1, psm, 0, 0, 64, 32}); // (the last walk drew over it)
    store.watch_reads();
    store.resolve(regs, 64, 32);
    store.upload_again(chunk);
    store.drawn(pcr::VramRect{page * 32, 1, psm, 0, 0, 64, 32});
    pcr::PagesRead walk = store.reads_watched();
    return expect_true(walk.pages[page] && store.unchanged(walk), "a walk's re-upload and draw over a drawn page changed its reads") && ok;
}

// A w x h rectangle at (x, y) of a buffer at bp (bw 1: a page's 64 x 32 32-bit pixels is one page of its own)
pcr::TransferRegs upload_regs_at(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return {bitbltbuf_of(bp, bw, psm), uint64_t(x) << 32 | uint64_t(y) << 48, uint64_t(w) | uint64_t(h) << 32};
}

// The reads are per block: a page part-written in a stretch is still read by a resolve of its other blocks, and an
// upload of a block the stretch's own writes owned before is read as the blocks it owned
bool partly_written_page() {
    Rng rng{25};
    pcr::TextureStore store;
    const uint32_t psm = GSN_PSMCT32, page = 70;
    store.upload(upload_regs(page * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); // the whole page, before the stretch
    store.watch_reads();
    store.upload(upload_regs_at(page * 32, 1, psm, 0, 16, 64, 16), bytes_from(rng, 4096)); // its bottom half, written
    pcr::TextureRegs top;
    top.tex0 = tex0_of(page * 32, 1, psm, 6, 4); // its top half, the earlier upload's
    bool ok = expect_true(store.resolve(top, 64, 16).source == TextureSource::FromUploads, "the top half is not the uploads'");
    pcr::PagesRead read = store.reads_watched();
    ok = expect_true(read.pages == pages_of_list({page}), "a page part-written, read in its other blocks, is not among the reads") && ok;
    // (not unchanged yet: the stretch's own upload over the bottom half is a change to what its owners read)
    store.upload(upload_regs_at(page * 32, 1, psm, 0, 0, 64, 16), bytes_from(rng, 4096)); // outside: the top half
    ok = expect_true(!store.unchanged(read), "an upload over the top half left the reads unchanged") && ok;

    // an upload over blocks with owners reads them first: the page is among the stretch's reads, though nothing else is read
    store.watch_reads();
    store.upload(upload_regs_at(page * 32, 1, psm, 0, 0, 64, 16), bytes_from(rng, 4096));
    pcr::PagesRead written_over = store.reads_watched();
    return expect_true(written_over.pages[page], "an upload over owned blocks did not read them first") && ok;
}

// ---- Resolve's answers kept ---------------------------------------------------------------------
// The same question again is answered kept while its pages answer the same, and walked once one changed: the answer
// every time the walking store's
bool kept_answers() {
    Rng rng{27};
    pcr::TextureStore store, walker(walking({}));
    const uint32_t psm = GSN_PSMCT32, page = 90;
    bool ok = true;
    auto both = [&](auto &&call) {
        call(store);
        call(walker);
    };
    auto ask = [&](const pcr::TextureRegs &regs, const char *what) {
        pcr::TextureAnswer a = store.resolve(regs, 64, 32), w = walker.resolve(regs, 64, 32);
        ok = expect_true(a.source == w.source && a.key == w.key && a.missing.block == w.missing.block, what) && ok;
        return a.source;
    };
    auto kept_since = [&](uint64_t before) {
        return store.counts().resolves_kept - before;
    };
    std::vector<uint8_t> bytes = bytes_from(rng, 8192);
    both([&](pcr::TextureStore &s) { s.upload(upload_regs(page * 32, 1, psm, 64, 32), bytes); });
    uint64_t chunk = store.last_chunk();
    pcr::TextureRegs regs;
    regs.tex0 = tex0_of(page * 32, 1, psm, 6, 5);

    uint64_t kept = store.counts().resolves_kept;
    ok = expect_true(ask(regs, "the first answer is not the walk's") == TextureSource::FromUploads, "an uploaded page is not the uploads'") && ok;
    ask(regs, "the same question again is not the walk's answer");
    ok = expect_true(kept_since(kept) == 1, "the same question again was not answered kept") && ok;

    kept = store.counts().resolves_kept;
    both([&](pcr::TextureStore &s) { s.upload(upload_regs(400 * 32, 1, psm, 64, 32), bytes_from(rng, 8192)); });
    ask(regs, "after an upload elsewhere, not the walk's answer");
    ok = expect_true(kept_since(kept) == 1, "an upload elsewhere turned the kept answer away") && ok;

    kept = store.counts().resolves_kept;
    both([&](pcr::TextureStore &s) { s.upload_again(chunk); });
    ask(regs, "after the same chunk again over itself, not the walk's answer");
    ok = expect_true(kept_since(kept) == 1, "the same chunk again over itself turned the kept answer away") && ok;

    kept = store.counts().resolves_kept;
    std::vector<uint8_t> other = bytes_from(rng, 4096);
    both([&](pcr::TextureStore &s) { s.upload(upload_regs_at(page * 32, 1, psm, 0, 16, 64, 16), other); });
    ask(regs, "after a different upload into the page, not the walk's answer");
    ok = expect_true(kept_since(kept) == 0, "a different upload into the page read was answered kept") && ok;

    kept = store.counts().resolves_kept;
    both([&](pcr::TextureStore &s) { s.drawn(pcr::VramRect{page * 32, 1, psm, 0, 0, 64, 32}); });
    ok = expect_true(ask(regs, "after a draw over the page, not the walk's answer") == TextureSource::DrawnSurface,
                     "a page drawn over is not DrawnSurface") && ok;
    ok = expect_true(kept_since(kept) == 0, "a draw over the page read was answered kept") && ok;
    ask(regs, "a DrawnSurface answer asked again is not the walk's");
    ok = expect_true(kept_since(kept) == 1, "a DrawnSurface answer asked again was not kept") && ok;

    kept = store.counts().resolves_kept;
    both([&](pcr::TextureStore &s) { s.upload(upload_regs(page * 32, 1, psm, 64, 32), bytes); });
    ok = expect_true(ask(regs, "after the page uploaded again, not the walk's answer") == TextureSource::FromUploads,
                     "a drawn page uploaded whole again is not the uploads'") && ok;
    ok = expect_true(kept_since(kept) == 0, "the page uploaded again over a draw was answered kept") && ok;

    // a page no upload owns: NotUploaded, then drawn into, DrawnSurface (its owners did not change, its draw marks did)
    pcr::TextureRegs empty;
    empty.tex0 = tex0_of(200 * 32, 1, psm, 6, 5);
    ok = expect_true(ask(empty, "a page never uploaded is not the walk's answer") == TextureSource::NotUploaded,
                     "a page never uploaded is not NotUploaded") && ok;
    both([&](pcr::TextureStore &s) { s.drawn(pcr::VramRect{200 * 32, 1, psm, 0, 0, 64, 32}); });
    ok = expect_true(ask(empty, "a page drawn into is not the walk's answer") == TextureSource::DrawnSurface,
                     "a page no upload owns, drawn into, is not DrawnSurface") && ok;

    // watched: a kept answer notes the blocks the walk read
    both([&](pcr::TextureStore &s) { s.watch_reads(); });
    kept = store.counts().resolves_kept;
    ask(regs, "a kept answer while watched is not the walk's");
    ok = expect_true(kept_since(kept) == 1, "the answer while watched was not kept") && ok;
    ok = expect_true(same_reads(store.reads_watched(), walker.reads_watched()), "a kept answer's reads are not the walk's") && ok;

    // forget: nothing kept
    both([&](pcr::TextureStore &s) { s.forget(); });
    kept = store.counts().resolves_kept;
    ask(regs, "after forget, not the walk's answer");
    return expect_true(kept_since(kept) == 0, "an answer kept from before forget was used") && ok;
}

} // namespace

int main(int argc, char **argv) {
    std::vector<uint64_t> seeds;
    for (int i = 1; i < argc; i++) {
        seeds.push_back(strtoull(argv[i], nullptr, 0));
    }
    if (seeds.empty()) {
        seeds = {1, 2, 3, 4, 5, 6};
    }
    bool ok = colour_texels_rules();
    ok = signatures_of_uploads() && ok;
    ok = watched_reads() && ok;
    ok = read_before_write() && ok;
    ok = clut_signatures() && ok;
    ok = reupload_in_stretch() && ok;
    ok = partly_written_page() && ok;
    ok = kept_answers() && ok;
    for (uint64_t seed : seeds) {
        ok = run_seed(seed) && ok;
        ok = vram_round_trip(seed) && ok;
        ok = pages_of_rules(seed) && ok;
    }
    printf("pcr_textures_test: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
