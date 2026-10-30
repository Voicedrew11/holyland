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
// pcr_textures_test [seed...]: the seeds to run (by default 1-6).
#include "pcr_textures.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct Run {
    Rng rng;
    pcr::TextureStore store;
    ReferenceGs gs;
    Tally tally;
    std::vector<Placed> placed;
    struct Sent {
        pcr::TransferRegs regs;
        std::vector<uint8_t> bytes;
        uint64_t chunk; // (the store's, 0 if it refused it)
    };
    std::vector<Sent> recent; // (uploads to send again)

    Run(uint64_t seed, pcr::TextureStoreLimits limits) : rng{seed * 0x9E3779B97F4A7C15ull + 1}, store(limits) {}

    void upload(const pcr::TransferRegs &regs, const std::vector<uint8_t> &bytes) {
        bool taken = store.upload(regs, bytes);
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
        std::vector<uint8_t> mine;
        if (!store.read_back(regs, mine)) {
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
        tally.copies++;
    }

    // The texture's answer checked: every texel of every level of the part read against the reference
    TextureSource check(const pcr::TextureRegs &regs, uint32_t cols_read, uint32_t rows_read) {
        pcr::TextureAnswer a = store.resolve(regs, cols_read, rows_read);
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
        pcr::TextureAnswer again = store.resolve(regs, cols_read, rows_read);
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
        store.load_clut(tex0, texclut);
        gs.load_clut(tex0, texclut);
        uint64_t texa = uint64_t(rng.below(256)) | uint64_t(rng.below(2)) << 15 | uint64_t(rng.below(256)) << 32;
        // the colours of this TEX0, and of another CSA's over the same buffer
        for (uint64_t read : {tex0, (tex0 & ~(uint64_t(31) << 56)) | uint64_t(rng.below(32)) << 56}) {
            pcr::ClutColours mine = store.clut_colours(read, texa);
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
        store.copied({~uint64_t(0), ~uint64_t(0), ~uint64_t(0)});
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
        gs.load_clut(clut0, 0x3FFFFF);
        store.clut_colours(~uint64_t(0), ~uint64_t(0));
    }

    void step() {
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
    pcr::TextureStore restored;
    restored.restore_vram(image);
    std::swap(run.store, restored);
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
           "CSM2, %llu of 16); store: %llu chunks kept, %llu decodes, %llu hits, %llu CLUT decodes, %llu refused\n",
           (unsigned long long)seed, (unsigned long long)t.uploads, (unsigned long long)t.sent_again, (unsigned long long)t.made_again,
           (unsigned long long)t.draws,
           (unsigned long long)t.copies, (unsigned long long)t.copies_checked, (unsigned long long)t.read_backs, (unsigned long long)t.forgets, (unsigned long long)t.answers[0],
           (unsigned long long)t.answers[1], (unsigned long long)t.answers[2], (unsigned long long)t.mipmapped,
           (unsigned long long)t.other_format, (unsigned long long)t.texels, (unsigned long long)t.clut_compared,
           (unsigned long long)t.clut_csm2, (unsigned long long)t.clut_16, (unsigned long long)c.chunks_kept,
           (unsigned long long)c.decodes, (unsigned long long)c.decode_hits, (unsigned long long)c.clut_decodes,
           (unsigned long long)c.uploads_refused);
    Tally check = t;
    bool met_all = t.answers[0] > 100 && t.answers[1] > 0 && t.answers[2] > 0 && t.mipmapped > 0 && t.other_format > 0 &&
                   t.clut_compared > 50 && t.sent_again > 0 && t.clut_csm2 > 0 && t.clut_16 > 0 && c.uploads_refused > 0;
    if (!met_all) {
        fail(check, "a kind of answer was not met: the test tests too little");
    }
    return check.failures == 0;
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
    bool ok = true;
    for (uint64_t seed : seeds) {
        ok = run_seed(seed) && ok;
        ok = vram_round_trip(seed) && ok;
    }
    printf("pcr_textures_test: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
