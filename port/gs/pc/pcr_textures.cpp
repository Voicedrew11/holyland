// The PC renderer's textures from their uploads (pcr_textures.h). The logic is the native GS's
// page-ownership table (port/gs/native/gsn_standalone.inc: sa_blocks_of, sa_own, sa_owners_walk,
// sa_table_texture, sa_scratch_write, sa_clut_load, sa_clut_colours) with its own state, and the GS's
// CLUT load (gsn.cpp's clut_load, clut_read_entries, clut_colours).
#include "pcr_textures.h"

#include <algorithm>
#include <bitset>
#include <cstring>
#include <list>
#include <unordered_map>

#include "gsn_vram.h"

namespace pcr {
namespace {

constexpr uint32_t kBlocks = GSN_VRAM_BYTES / 256;
constexpr uint32_t kBlocksPerPage = 32;
constexpr uint32_t kPages = kBlocks / kBlocksPerPage;
constexpr uint32_t kWrap = 2048; // (a transfer's and a buffer's coordinates wrap here)
constexpr uint32_t kMaxLevels = 7;
constexpr uint32_t kMaxSizeLog2 = 10;
constexpr uint32_t kNoBlock = UINT32_MAX;

uint32_t field(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

bool is_16bit(uint32_t psm) {
    return psm == GSN_PSMCT16 || psm == GSN_PSMCT16S || psm == GSN_PSMZ16 || psm == GSN_PSMZ16S;
}

bool is_palette(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 || psm == GSN_PSMT8H || psm == GSN_PSMT4HL || psm == GSN_PSMT4HH;
}

bool has_256_entries(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT8H;
}

// A page of a format: this many pixels across and down
uint32_t page_width_px(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 ? 128 : 64;
}

uint32_t page_height_px(uint32_t psm) {
    if (psm == GSN_PSMT4) {
        return 128;
    }
    return psm == GSN_PSMT8 || is_16bit(psm) ? 64 : 32;
}

uint32_t block_width_px(uint32_t psm) {
    return psm == GSN_PSMT4 ? 32 : psm == GSN_PSMT8 || is_16bit(psm) ? 16 : 8;
}

uint32_t block_height_px(uint32_t psm) {
    return psm == GSN_PSMT8 || psm == GSN_PSMT4 ? 16 : 8;
}

// ---- Nibbles of a block ---------------------------------------------------------------------------
// Which nibbles of a block's 64 words: bit w * 8 + n is nibble n (bits 4n..4n+3) of word w
using Cover = std::array<uint64_t, 8>;

constexpr uint8_t kLowParts = 0x3F, kTopParts = 0xC0; // (the nibbles of the low 24 bits, of the top byte)

bool any(const Cover &c) {
    return (c[0] | c[1] | c[2] | c[3] | c[4] | c[5] | c[6] | c[7]) != 0;
}

Cover both(const Cover &a, const Cover &b) {
    Cover c;
    for (size_t i = 0; i < c.size(); i++) {
        c[i] = a[i] & b[i];
    }
    return c;
}

// The nibbles of any word the cover has
uint8_t word_parts_in(const Cover &c) {
    uint64_t f = c[0] | c[1] | c[2] | c[3] | c[4] | c[5] | c[6] | c[7];
    f |= f >> 32;
    f |= f >> 16;
    f |= f >> 8;
    return uint8_t(f);
}

// The nibbles of each word a format's pixels own
uint8_t word_parts(uint32_t psm) {
    switch (psm) {
    case GSN_PSMCT24:
    case GSN_PSMZ24:
        return kLowParts;
    case GSN_PSMT8H:
        return kTopParts;
    case GSN_PSMT4HL:
        return 0x40;
    case GSN_PSMT4HH:
        return 0x80;
    default:
        return 0xFF;
    }
}

// The nibbles of its word the pixel at byte `at` of a block owns (nibble: gsn_vram_offset's)
uint8_t pixel_parts(uint32_t psm, uint32_t at, int nibble) {
    if (is_16bit(psm)) {
        return (at & 2) ? 0xF0 : 0x0F;
    }
    if (psm == GSN_PSMT8) {
        return uint8_t(3u << ((at & 3) * 2));
    }
    if (psm == GSN_PSMT4) {
        return uint8_t(1u << ((at & 3) * 2 + (nibble > 0 ? 1 : 0)));
    }
    return word_parts(psm);
}

// ---- Rectangles of VRAM ---------------------------------------------------------------------------
// A rectangle as the GS has it: its fields in their registers' widths, its corner wrapped, no wider or
// taller than the wrap (past that its pixels are those it has already)
VramRect wrapped(VramRect r) {
    r.bp &= 0x3FFF;
    r.bw &= 0x3F;
    r.x %= kWrap;
    r.y %= kWrap;
    r.width_px = std::min(r.width_px, kWrap);
    r.height_px = std::min(r.height_px, kWrap);
    return r;
}

// Visits the blocks a wrapped rectangle's pixels are in: visit(block, x, y) with the block's first pixel in
// the rectangle's coordinates (not wrapped); visit returns false to stop
template <class Visit> void each_block(const VramRect &r, Visit &&visit) {
    uint32_t bx = block_width_px(r.psm), by = block_height_px(r.psm);
    for (uint32_t y = r.y / by * by; y < r.y + r.height_px; y += by) {
        for (uint32_t x = r.x / bx * bx; x < r.x + r.width_px; x += bx) {
            uint32_t block = (gsn_vram_offset(r.psm, r.bp, r.bw, x % kWrap, y % kWrap, nullptr) / 256) % kBlocks;
            if (!visit(block, x, y)) {
                return;
            }
        }
    }
}

// The nibbles of the block at (x, y) (each_block's) that the rectangle's pixels are
Cover cover_in_block(const VramRect &r, uint32_t x, uint32_t y) {
    uint32_t bx = block_width_px(r.psm), by = block_height_px(r.psm);
    Cover c{};
    if (x >= r.x && x + bx <= r.x + r.width_px && y >= r.y && y + by <= r.y + r.height_px) {
        c.fill(uint64_t(word_parts(r.psm)) * 0x0101010101010101ull);
        return c;
    }
    for (uint32_t py = std::max(y, r.y); py < std::min(y + by, r.y + r.height_px); py++) {
        for (uint32_t px = std::max(x, r.x); px < std::min(x + bx, r.x + r.width_px); px++) {
            int nibble = -1;
            uint32_t at = gsn_vram_offset(r.psm, r.bp, r.bw, px % kWrap, py % kWrap, &nibble) % 256;
            uint32_t word = at / 4;
            c[word / 8] |= uint64_t(pixel_parts(r.psm, at, nibble)) << (word % 8 * 8);
        }
    }
    return c;
}

// ---- Hashing --------------------------------------------------------------------------------------
uint64_t mix(uint64_t h, uint64_t v) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

uint64_t hash_bytes(uint64_t h, std::span<const uint8_t> bytes) {
    size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        uint64_t v;
        memcpy(&v, bytes.data() + i, 8);
        h = mix(h, v);
    }
    for (; i < bytes.size(); i++) {
        h = mix(h, bytes[i]);
    }
    return mix(h, bytes.size());
}

// ---- Texels ---------------------------------------------------------------------------------------
// A pixel as gsn_vram_read gives it, as the texture's RGBA8 (pcr_textures.h, DecodedTexture)
uint32_t rgba_of(uint32_t psm, uint32_t v) {
    if (is_16bit(psm)) {
        uint32_t r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
        uint32_t a = (v & 0x8000) ? 0xFF : 0;
        return r | g << 8 | b << 16 | a << 24;
    }
    if (psm == GSN_PSMCT24 || psm == GSN_PSMZ24) {
        return v & 0xFFFFFF;
    }
    return v;
}

uint32_t clut_psm(uint32_t cpsm) {
    return cpsm == 0 ? GSN_PSMCT32 : cpsm == 2 ? GSN_PSMCT16 : GSN_PSMCT16S;
}

// The rectangle a CLUT load reads (CSM1: 16 x 16 or 8 x 2 at the buffer's corner; CSM2: TEXCLUT's line)
VramRect clut_rect(uint64_t tex0, uint64_t texclut) {
    uint32_t psm = field(tex0, 20, 6), cbp = field(tex0, 37, 14), gpsm = clut_psm(field(tex0, 51, 4));
    bool eight = has_256_entries(psm);
    if (field(tex0, 55, 1) == 0) {
        return {cbp, 1, gpsm, 0, 0, eight ? 16u : 8u, eight ? 16u : 2u};
    }
    return {cbp, field(texclut, 0, 6), gpsm, field(texclut, 6, 6) * 16, field(texclut, 12, 10), eight ? 256u : 16u, 1};
}

// The entries a CLUT load reads from a VRAM, as its format has them (CSM1: entries 8-15 and 16-23 of each
// 32 swapped)
std::array<uint32_t, 256> read_clut_entries(const uint8_t *vram, uint64_t tex0, uint64_t texclut) {
    std::array<uint32_t, 256> values{};
    VramRect r = clut_rect(tex0, texclut);
    bool eight = has_256_entries(field(tex0, 20, 6));
    if (field(tex0, 55, 1) != 0) {
        gsn_vram_read_row(vram, r.psm, r.bp, r.bw, r.x, r.y, r.width_px, values.data());
        return values;
    }
    std::array<uint32_t, 256> grid{};
    for (uint32_t y = 0; y < r.height_px; y++) {
        gsn_vram_read_row(vram, r.psm, r.bp, 1, 0, y, r.width_px, grid.data() + y * r.width_px);
    }
    uint32_t entries = eight ? 256 : 16;
    for (uint32_t i = 0; i < entries; i++) {
        values[i] = grid[eight ? ((i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)) : i];
    }
    return values;
}

// Where entry i of a load lands in the CLUT buffer's 512 halfwords: CT32's low half here, high half 256 on
uint32_t clut_halfword(uint64_t tex0, uint32_t i) {
    bool eight = has_256_entries(field(tex0, 20, 6));
    uint32_t csa = field(tex0, 56, 5);
    if (field(tex0, 51, 4) == 0) {
        return eight ? i : ((csa & 15) * 16 + i) & 0xFF;
    }
    return eight ? i : (csa * 16 + i) & 0x1FF;
}

struct WordsHash {
    size_t operator()(const std::vector<uint64_t> &words) const {
        uint64_t h = 0;
        for (uint64_t w : words) {
            h = mix(h, w);
        }
        return size_t(h);
    }
};

struct KeyHash {
    size_t operator()(const TextureKey &k) const {
        return size_t(k.hash());
    }
};

} // namespace

std::bitset<512> pages_of(const VramRect &rect) {
    VramRect r = rect;
    if (!gsn_psm_valid(r.psm)) {
        r.psm = GSN_PSMCT32;
    }
    r = wrapped(r);
    std::bitset<kPages> pages;
    if (r.width_px == 0 || r.height_px == 0) {
        return pages;
    }
    if (r.bp % kBlocksPerPage != 0) {
        each_block(r, [&](uint32_t block, uint32_t, uint32_t) {
            pages.set(block / kBlocksPerPage);
            return true;
        });
        return pages;
    }
    // (a pixel's page is the base's, on by its row of pages and its column, as gsn_vram.c's pixel_nibble has it: the
    // blocks within a page, the Z formats' arrangement too, stay in it)
    uint32_t pw = page_width_px(r.psm), ph = page_height_px(r.psm);
    uint32_t stride = pw == 128 ? r.bw >> 1 : r.bw;
    for (uint32_t y = r.y / ph * ph; y < r.y + r.height_px; y += ph) {
        for (uint32_t x = r.x / pw * pw; x < r.x + r.width_px; x += pw) {
            pages.set((r.bp / kBlocksPerPage + y % kWrap / ph * stride + x % kWrap / pw) % kPages);
        }
    }
    return pages;
}

uint64_t TextureKey::hash() const {
    uint64_t h = mix(0, uint64_t(tbp) | uint64_t(tbw) << 14 | uint64_t(psm) << 20 | uint64_t(tw_log2) << 26 |
                            uint64_t(th_log2) << 30 | uint64_t(levels) << 34);
    h = mix(h, uint64_t(read_cols) | uint64_t(read_rows) << 32);
    for (uint32_t l = 1; l < levels && l < kMaxLevels; l++) {
        h = mix(h, uint64_t(level_tbp[l]) | uint64_t(level_tbw[l]) << 14);
    }
    for (uint64_t id : chunks) {
        h = mix(h, id);
    }
    return h;
}

uint32_t DecodedTexture::level_width(uint32_t level) const {
    return std::max(1u, width_px >> level);
}

uint32_t DecodedTexture::level_height(uint32_t level) const {
    return std::max(1u, height_px >> level);
}

std::vector<uint32_t> colour_texels(const DecodedTexture &texture, uint32_t psm, const ClutColours &clut, uint64_t texa) {
    if (texture.kind == TexelKind::Index8) {
        std::vector<uint32_t> out(texture.indices.size());
        for (size_t i = 0; i < out.size(); i++) {
            out[i] = clut.rgba[texture.indices[i]];
        }
        return out;
    }
    std::vector<uint32_t> out = texture.rgba;
    bool ct24 = psm == 0x01 || psm == 0x31;
    bool ct16 = psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A;
    if (!ct24 && !ct16) {
        return out;
    }
    uint32_t ta0 = uint32_t(texa & 0xFF), ta1 = uint32_t((texa >> 32) & 0xFF);
    bool aem = ((texa >> 15) & 1) != 0;
    for (uint32_t &c : out) {
        uint32_t rgb = c & 0xFFFFFFu;
        if (ct16 && (c >> 24) != 0) {
            c = rgb | ta1 << 24;
        } else {
            c = rgb | (aem && rgb == 0 ? 0u : ta0) << 24;
        }
    }
    return out;
}

// ---- The store's state ----------------------------------------------------------------------------
namespace {

// An upload, kept: its registers (the destination's fields only), its bytes (those its pixels take)
struct Chunk {
    TransferRegs regs;
    std::vector<uint8_t> bytes;
    uint64_t pixels = 0;
    uint64_t hash = 0;
    uint64_t seq = 0;           // the store's count when it was last uploaded
    uint64_t owner_entries = 0; // blocks' owner entries naming it (none: it owns nothing)
    std::optional<std::list<uint64_t>::iterator> unowned_at;
};

// One of a block's uploads, with the nibbles still its own
struct Owner {
    uint64_t chunk;
    uint64_t seq; // the store's count when it was written
    Cover cover;
};

// The uploads a rectangle's nibbles are, in the order met, or where it is not the uploads'
struct Owners {
    TextureSource source = TextureSource::FromUploads;
    MissingBlock missing;
    std::vector<uint64_t> chunks;
};

// The rectangles of an upload's pixels: its whole rows, then the row it ended in the middle of
std::array<VramRect, 2> upload_rects(const TransferRegs &regs, uint64_t pixels) {
    VramRect whole{field(regs.bitbltbuf, 32, 14), field(regs.bitbltbuf, 48, 6), field(regs.bitbltbuf, 56, 6),
                   field(regs.trxpos, 32, 11),    field(regs.trxpos, 48, 11),   field(regs.trxreg, 0, 12),
                   0};
    uint32_t width = whole.width_px;
    VramRect rest = whole;
    whole.height_px = uint32_t(pixels / width);
    rest.y += whole.height_px;
    rest.width_px = uint32_t(pixels % width);
    rest.height_px = 1;
    return {wrapped(whole), wrapped(rest)};
}

// The destination's fields of a transfer: what makes two uploads of the same bytes the same
TransferRegs destination_of(const TransferRegs &regs) {
    return {regs.bitbltbuf & 0x3F3FFFFF00000000ull, regs.trxpos & 0x07FF07FF00000000ull,
            regs.trxreg & 0x00000FFF00000FFFull};
}

bool same_regs(const TransferRegs &a, const TransferRegs &b) {
    return a.bitbltbuf == b.bitbltbuf && a.trxpos == b.trxpos && a.trxreg == b.trxreg;
}

} // namespace

struct TextureStore::State {
    TextureStoreLimits limits;
    TextureStoreCounts counts;
    uint64_t seq = 0, next_chunk = 1, last_chunk = 0;
    std::vector<std::vector<Owner>> block_owners = std::vector<std::vector<Owner>>(kBlocks);
    // the count when a page was last drawn into or copied over, in its low 24 bits and in its top byte
    std::array<uint64_t, kPages> drawn_low_at{}, drawn_top_at{};
    std::unordered_map<uint64_t, Chunk> chunks;
    std::unordered_multimap<uint64_t, uint64_t> chunk_by_hash;
    std::list<uint64_t> unowned; // (chunks owning nothing, oldest first)
    size_t unowned_bytes = 0, chunk_bytes = 0;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(GSN_VRAM_BYTES); // the VRAM a texture is decoded in
    // decoded textures, most recently used first
    using Decoded = std::pair<TextureKey, std::shared_ptr<const DecodedTexture>>;
    std::list<Decoded> decoded;
    std::unordered_map<TextureKey, std::list<Decoded>::iterator, KeyHash> decoded_by_key;
    size_t decoded_bytes = 0;
    // the CLUT buffer mirrored: its 512 halfwords, those not loaded from uploads, the CLD's CBP0/CBP1
    std::array<uint16_t, 512> clut{};
    std::bitset<512> clut_unloaded = std::bitset<512>().set();
    std::array<uint32_t, 2> clut_cbp = {UINT32_MAX, UINT32_MAX};
    std::unordered_map<std::vector<uint64_t>, std::array<uint32_t, 256>, WordsHash> clut_decoded;

    uint64_t keep(const TransferRegs &regs, std::span<const uint8_t> bytes, uint64_t pixels);
    void own(uint64_t id);
    void lose_entry(uint64_t id, std::vector<uint64_t> &now_unowned);
    void mark_unowned(uint64_t id);
    void mark_owned(Chunk &c);
    void let_go_unowned();
    Owners owners_of(const VramRect &r, uint32_t level) const;
    void write_chunks(const std::vector<uint64_t> &ids, std::span<const VramRect> zeroed);
    std::shared_ptr<const DecodedTexture> decode(const TextureKey &key);
    void keep_decoded(const TextureKey &key, std::shared_ptr<const DecodedTexture> texture);
    std::array<uint32_t, 256> clut_from(const std::vector<uint64_t> &ids, uint64_t tex0, uint64_t texclut);
    void sort_by_upload(std::vector<uint64_t> &ids) const;
};

// The chunk an upload is: found again (the same bytes to the same place), or kept new
uint64_t TextureStore::State::keep(const TransferRegs &regs, std::span<const uint8_t> bytes, uint64_t pixels) {
    uint64_t hash = hash_bytes(mix(mix(mix(0, regs.bitbltbuf), regs.trxpos), regs.trxreg), bytes);
    auto range = chunk_by_hash.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
        const Chunk &c = chunks.at(it->second);
        if (same_regs(c.regs, regs) && std::equal(c.bytes.begin(), c.bytes.end(), bytes.begin(), bytes.end())) {
            return it->second;
        }
    }
    uint64_t id = next_chunk++;
    Chunk &c = chunks[id];
    c.regs = regs;
    c.bytes.assign(bytes.begin(), bytes.end());
    c.pixels = pixels;
    c.hash = hash;
    chunk_by_hash.emplace(hash, id);
    chunk_bytes += bytes.size();
    counts.chunks_new++;
    return id;
}

// The chunk's nibbles are its own: taken from the uploads before it, which keep the rest
void TextureStore::State::own(uint64_t id) {
    Chunk &c = chunks.at(id);
    c.seq = ++seq;
    std::vector<uint64_t> now_unowned;
    for (const VramRect &r : upload_rects(c.regs, c.pixels)) {
        each_block(r, [&](uint32_t block, uint32_t x, uint32_t y) {
            Cover cover = cover_in_block(r, x, y);
            std::vector<Owner> &owners = block_owners[block];
            size_t kept = 0;
            for (Owner &o : owners) {
                for (size_t k = 0; k < cover.size(); k++) {
                    o.cover[k] &= ~cover[k];
                }
                if (any(o.cover)) {
                    owners[kept++] = o;
                } else {
                    lose_entry(o.chunk, now_unowned);
                }
            }
            owners.resize(kept);
            owners.push_back({id, c.seq, cover});
            c.owner_entries++;
            return true;
        });
    }
    if (c.owner_entries > 0) {
        mark_owned(c);
    }
    for (uint64_t lost : now_unowned) {
        mark_unowned(lost);
    }
    let_go_unowned();
}

void TextureStore::State::lose_entry(uint64_t id, std::vector<uint64_t> &now_unowned) {
    Chunk &c = chunks.at(id);
    if (--c.owner_entries == 0) {
        now_unowned.push_back(id);
    }
}

// A chunk that owns nothing (any more): kept, to be found again, while those like it fit the limit
void TextureStore::State::mark_unowned(uint64_t id) {
    Chunk &c = chunks.at(id);
    if (c.owner_entries > 0 || c.unowned_at) {
        return;
    }
    c.unowned_at = unowned.insert(unowned.end(), id);
    unowned_bytes += c.bytes.size();
}

void TextureStore::State::mark_owned(Chunk &c) {
    if (!c.unowned_at) {
        return;
    }
    unowned.erase(*c.unowned_at);
    c.unowned_at.reset();
    unowned_bytes -= c.bytes.size();
}

// The oldest chunks that own nothing let go beyond the limit (no block names them)
void TextureStore::State::let_go_unowned() {
    while (unowned_bytes > limits.unowned_chunk_bytes && !unowned.empty()) {
        uint64_t id = unowned.front();
        unowned.pop_front();
        const Chunk &c = chunks.at(id);
        unowned_bytes -= c.bytes.size();
        chunk_bytes -= c.bytes.size();
        auto range = chunk_by_hash.equal_range(c.hash);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == id) {
                chunk_by_hash.erase(it);
                break;
            }
        }
        chunks.erase(id);
    }
}

// The uploads a wrapped rectangle's nibbles are: every nibble an upload's, and that upload's page not drawn
// into or copied over since in that nibble
Owners TextureStore::State::owners_of(const VramRect &r, uint32_t level) const {
    Owners found;
    each_block(r, [&](uint32_t block, uint32_t x, uint32_t y) {
        Cover need = cover_in_block(r, x, y);
        uint32_t page = block / kBlocksPerPage;
        Cover covered{};
        for (const Owner &o : block_owners[block]) {
            Cover theirs = both(o.cover, need);
            if (!any(theirs)) {
                continue;
            }
            uint8_t parts = word_parts_in(theirs);
            if (((parts & kLowParts) && drawn_low_at[page] > o.seq) || ((parts & kTopParts) && drawn_top_at[page] > o.seq)) {
                found.source = TextureSource::DrawnSurface;
                found.missing = {level, block};
                return false;
            }
            if (std::find(found.chunks.begin(), found.chunks.end(), o.chunk) == found.chunks.end()) {
                found.chunks.push_back(o.chunk);
            }
            for (size_t k = 0; k < covered.size(); k++) {
                covered[k] |= theirs[k];
            }
        }
        if (covered != need) {
            uint8_t parts = word_parts_in(need);
            bool drawn = ((parts & kLowParts) && drawn_low_at[page]) || ((parts & kTopParts) && drawn_top_at[page]);
            found.source = drawn ? TextureSource::DrawnSurface : TextureSource::NotUploaded;
            found.missing = {level, block};
            return false;
        }
        return true;
    });
    return found;
}

// Oldest first: the order they were last uploaded in
void TextureStore::State::sort_by_upload(std::vector<uint64_t> &ids) const {
    std::sort(ids.begin(), ids.end(), [&](uint64_t a, uint64_t b) { return chunks.at(a).seq < chunks.at(b).seq; });
}

// The chunks (oldest first) written into the scratch VRAM, over the rectangles' blocks made zero
void TextureStore::State::write_chunks(const std::vector<uint64_t> &ids, std::span<const VramRect> zeroed) {
    for (const VramRect &r : zeroed) {
        each_block(r, [&](uint32_t block, uint32_t, uint32_t) {
            memset(scratch.data() + size_t(block) * 256, 0, 256);
            return true;
        });
    }
    for (uint64_t id : ids) {
        const Chunk &c = chunks.at(id);
        GsnUpload u;
        gsn_upload_begin(&u, c.regs.bitbltbuf, c.regs.trxpos, c.regs.trxreg);
        gsn_upload_data(&u, scratch.data(), c.bytes.data(), c.bytes.size(), nullptr, nullptr);
    }
}

// A texture's levels written from its chunks and read back as its format
std::shared_ptr<const DecodedTexture> TextureStore::State::decode(const TextureKey &key) {
    std::array<VramRect, kMaxLevels> levels;
    for (uint32_t l = 0; l < key.levels; l++) {
        levels[l] = wrapped({key.level_tbp[l], key.level_tbw[l], key.psm, 0, 0, std::max(1u, (1u << key.tw_log2) >> l),
                             std::max(1u, (1u << key.th_log2) >> l)});
    }
    write_chunks(key.chunks, std::span<const VramRect>(levels.data(), key.levels));

    auto t = std::make_shared<DecodedTexture>();
    t->kind = is_palette(key.psm) ? TexelKind::Index8 : TexelKind::Rgba8;
    t->width_px = 1u << key.tw_log2;
    t->height_px = 1u << key.th_log2;
    t->levels = key.levels;
    t->read_cols = key.read_cols;
    t->read_rows = key.read_rows;
    size_t texels = 0;
    for (uint32_t l = 0; l < key.levels; l++) {
        t->level_start[l] = texels;
        texels += size_t(t->level_width(l)) * t->level_height(l);
    }
    if (t->kind == TexelKind::Index8) {
        t->indices.resize(texels);
    } else {
        t->rgba.resize(texels);
    }
    std::vector<uint32_t> row;
    for (uint32_t l = 0; l < key.levels; l++) {
        uint32_t w = t->level_width(l), h = t->level_height(l);
        row.resize(w);
        for (uint32_t y = 0; y < h; y++) {
            gsn_vram_read_row(scratch.data(), key.psm, key.level_tbp[l], key.level_tbw[l], 0, y, w, row.data());
            size_t at = t->level_start[l] + size_t(y) * w;
            for (uint32_t x = 0; x < w; x++) {
                if (t->kind == TexelKind::Index8) {
                    t->indices[at + x] = uint8_t(row[x]);
                } else {
                    t->rgba[at + x] = rgba_of(key.psm, row[x]);
                }
            }
        }
    }
    counts.decodes++;
    return t;
}

// Kept as the most recently used; the least recently used let go beyond the limit (but the newest)
void TextureStore::State::keep_decoded(const TextureKey &key, std::shared_ptr<const DecodedTexture> texture) {
    decoded_bytes += texture->rgba.size() * 4 + texture->indices.size();
    decoded.emplace_front(key, std::move(texture));
    decoded_by_key[key] = decoded.begin();
    while (decoded_bytes > limits.decoded_bytes && decoded.size() > 1) {
        const Decoded &oldest = decoded.back();
        decoded_bytes -= oldest.second->rgba.size() * 4 + oldest.second->indices.size();
        decoded_by_key.erase(oldest.first);
        decoded.pop_back();
    }
}

// A CLUT load's entries read from its uploads (sorted oldest first), decoded once per load and uploads
std::array<uint32_t, 256> TextureStore::State::clut_from(const std::vector<uint64_t> &ids, uint64_t tex0,
                                                         uint64_t texclut) {
    bool csm2 = field(tex0, 55, 1) != 0;
    // (what the entries are: CBP, CPSM, CSM, 16 or 256, TEXCLUT for CSM2, and the uploads)
    std::vector<uint64_t> key = {
        (tex0 & 0x00FFFFE000000000ull) | uint64_t(has_256_entries(field(tex0, 20, 6))),
        csm2 ? (texclut & 0x3FFFFF) : 0};
    key.insert(key.end(), ids.begin(), ids.end());
    auto found = clut_decoded.find(key);
    if (found != clut_decoded.end()) {
        counts.clut_hits++;
        return found->second;
    }
    VramRect r = wrapped(clut_rect(tex0, texclut));
    write_chunks(ids, std::span<const VramRect>(&r, 1));
    std::array<uint32_t, 256> values = read_clut_entries(scratch.data(), tex0, texclut);
    if (clut_decoded.size() >= 4096) {
        clut_decoded.clear();
    }
    clut_decoded.emplace(std::move(key), values);
    counts.clut_decodes++;
    return values;
}

// ---- The interface --------------------------------------------------------------------------------
TextureStore::TextureStore(TextureStoreLimits limits) : state_(std::make_unique<State>()) {
    state_->limits = limits;
}

TextureStore::~TextureStore() = default;
TextureStore::TextureStore(TextureStore &&) noexcept = default;
TextureStore &TextureStore::operator=(TextureStore &&) noexcept = default;

bool TextureStore::upload(const TransferRegs &regs, std::span<const uint8_t> bytes) {
    State &s = *state_;
    TransferRegs dest = destination_of(regs);
    uint32_t psm = field(dest.bitbltbuf, 56, 6);
    uint64_t width = field(dest.trxreg, 0, 12), height = field(dest.trxreg, 32, 12);
    int bits = gsn_psm_valid(psm) ? gsn_psm_bits(psm) : 0;
    uint64_t pixels = bits ? std::min(width * height, uint64_t(bytes.size()) * 8 / uint64_t(bits)) : 0;
    if (pixels == 0) {
        s.counts.uploads_refused++;
        return false;
    }
    std::span<const uint8_t> used = bytes.first(size_t((pixels * uint64_t(bits) + 7) / 8));
    s.counts.uploads++;
    s.counts.upload_bytes += used.size();
    s.last_chunk = s.keep(dest, used, pixels);
    s.own(s.last_chunk);
    return true;
}

uint64_t TextureStore::last_chunk() const {
    return state_->last_chunk;
}

bool TextureStore::upload_again(uint64_t chunk) {
    State &s = *state_;
    auto found = s.chunks.find(chunk);
    if (found == s.chunks.end()) {
        return false;
    }
    s.counts.uploads++;
    s.counts.uploads_again++;
    s.counts.upload_bytes += found->second.bytes.size();
    s.last_chunk = chunk;
    s.own(chunk);
    return true;
}

void TextureStore::drawn(const VramRect &rect) {
    State &s = *state_;
    VramRect r = rect;
    if (!gsn_psm_valid(r.psm)) {
        r.psm = GSN_PSMCT32; // (a format the GS does not have: all of each word taken as drawn)
    }
    uint8_t parts = word_parts(r.psm);
    uint64_t now = ++s.seq;
    std::bitset<kPages> pages = pages_of(r);
    for (uint32_t page = 0; page < kPages; page++) {
        if (!pages[page]) {
            continue;
        }
        if (parts & kLowParts) {
            s.drawn_low_at[page] = now;
        }
        if (parts & kTopParts) {
            s.drawn_top_at[page] = now;
        }
    }
}

void TextureStore::copied(const TransferRegs &regs) {
    State &s = *state_;
    uint32_t width = field(regs.trxreg, 0, 12), height = field(regs.trxreg, 32, 12);
    uint32_t spsm = field(regs.bitbltbuf, 24, 6), dpsm = field(regs.bitbltbuf, 56, 6);
    VramRect dest = {field(regs.bitbltbuf, 32, 14), field(regs.bitbltbuf, 48, 6), dpsm, field(regs.trxpos, 32, 11),
                     field(regs.trxpos, 48, 11), width, height};
    if (width == 0 || height == 0 || !gsn_psm_valid(spsm) || !gsn_psm_valid(dpsm)) {
        drawn(dest);
        return;
    }
    // A copy of what uploads hold is an upload of what it writes: the source's chunks written into the scratch
    // VRAM, the copy made there as the GS makes it, and the destination's pixels read back as its upload
    Owners owners = s.owners_of(wrapped({field(regs.bitbltbuf, 0, 14), field(regs.bitbltbuf, 16, 6), spsm,
                                         field(regs.trxpos, 0, 11), field(regs.trxpos, 16, 11), width, height}),
                                0);
    if (owners.source != TextureSource::FromUploads) {
        drawn(dest);
        return;
    }
    s.sort_by_upload(owners.chunks);
    s.write_chunks(owners.chunks, {});
    gsn_vram_copy(s.scratch.data(), regs.bitbltbuf, regs.trxpos, regs.trxreg);
    size_t bytes = (size_t(width) * height * uint32_t(gsn_psm_bits(dpsm)) + 7) / 8;
    std::vector<uint8_t> pixels((bytes + 15) / 16 * 16);
    uint64_t read_from = regs.bitbltbuf >> 32; // (the destination's DBP, DBW and DPSM, as a read's source)
    uint64_t read_at = regs.trxpos >> 32;      // (DSAX and DSAY as SSAX and SSAY; read top down, left to right)
    gsn_vram_read_image(s.scratch.data(), read_from, read_at & 0x07FF07FFull, regs.trxreg, pixels.data(),
                        pixels.size() / 16);
    pixels.resize(bytes);
    s.counts.copies_as_uploads++;
    upload(regs, pixels);
}

bool TextureStore::read_back(const TransferRegs &regs, std::vector<uint8_t> &out) {
    State &s = *state_;
    out.clear();
    uint32_t width = field(regs.trxreg, 0, 12), height = field(regs.trxreg, 32, 12);
    uint32_t spsm = field(regs.bitbltbuf, 24, 6);
    if (width == 0 || height == 0 || !gsn_psm_valid(spsm)) {
        s.counts.read_backs_refused++;
        return false;
    }
    Owners owners = s.owners_of(wrapped({field(regs.bitbltbuf, 0, 14), field(regs.bitbltbuf, 16, 6), spsm,
                                         field(regs.trxpos, 0, 11), field(regs.trxpos, 16, 11), width, height}),
                                0);
    if (owners.source != TextureSource::FromUploads) {
        s.counts.read_backs_refused++;
        return false;
    }
    s.sort_by_upload(owners.chunks);
    s.write_chunks(owners.chunks, {});
    size_t bytes = (size_t(width) * height * uint32_t(gsn_psm_bits(spsm)) + 7) / 8;
    out.resize((bytes + 15) / 16 * 16);
    gsn_vram_read_image(s.scratch.data(), regs.bitbltbuf, regs.trxpos, regs.trxreg, out.data(), out.size() / 16);
    s.counts.read_backs++;
    return true;
}

TextureAnswer TextureStore::resolve(const TextureRegs &regs, uint32_t cols_read, uint32_t rows_read) {
    State &s = *state_;
    s.counts.resolves++;
    TextureAnswer answer;
    TextureKey &key = answer.key;
    key.tbp = field(regs.tex0, 0, 14);
    key.tbw = field(regs.tex0, 14, 6);
    key.psm = field(regs.tex0, 20, 6);
    key.tw_log2 = std::min(field(regs.tex0, 26, 4), kMaxSizeLog2);
    key.th_log2 = std::min(field(regs.tex0, 30, 4), kMaxSizeLog2);
    if (!gsn_psm_valid(key.psm)) {
        answer.missing = {0, kNoBlock};
        return answer;
    }
    // the levels (TEX1's MMIN a mipmap filter and MXL some), at most as many as halve the larger side to 1
    key.levels = 1;
    key.level_tbp[0] = key.tbp;
    key.level_tbw[0] = key.tbw;
    uint32_t mmin = field(regs.tex1, 6, 3), mxl = field(regs.tex1, 2, 3);
    if (mmin >= 2 && mmin <= 5 && mxl > 0) {
        key.levels = std::min({std::min(mxl, kMaxLevels - 1) + 1, std::max(key.tw_log2, key.th_log2) + 1});
        for (uint32_t l = 1; l < key.levels; l++) {
            uint64_t m = l <= 3 ? regs.miptbp1 : regs.miptbp2;
            int at = int((l - 1) % 3) * 20;
            key.level_tbp[l] = field(m, at, 14);
            key.level_tbw[l] = field(m, at + 14, 6);
        }
    }
    // the part of level 0 read: without mipmaps, and not REGION_REPEAT, the texels read in whole pages
    key.read_cols = 1u << key.tw_log2;
    key.read_rows = 1u << key.th_log2;
    if (key.levels == 1 && field(regs.clamp, 0, 2) != 3) {
        uint32_t pw = page_width_px(key.psm);
        key.read_cols = std::min(key.read_cols, (std::clamp(cols_read, 1u, key.read_cols) + pw - 1) / pw * pw);
    }
    if (key.levels == 1 && field(regs.clamp, 2, 2) != 3) {
        uint32_t ph = page_height_px(key.psm);
        key.read_rows = std::min(key.read_rows, (std::clamp(rows_read, 1u, key.read_rows) + ph - 1) / ph * ph);
    }
    for (uint32_t l = 0; l < key.levels; l++) {
        VramRect r = wrapped({key.level_tbp[l], key.level_tbw[l], key.psm, 0, 0, std::max(1u, key.read_cols >> l),
                              std::max(1u, key.read_rows >> l)});
        Owners owners = s.owners_of(r, l);
        if (owners.source != TextureSource::FromUploads) {
            answer.source = owners.source;
            answer.missing = owners.missing;
            key = TextureKey{};
            return answer;
        }
        for (uint64_t id : owners.chunks) {
            if (std::find(key.chunks.begin(), key.chunks.end(), id) == key.chunks.end()) {
                key.chunks.push_back(id);
            }
        }
    }
    s.sort_by_upload(key.chunks);
    answer.source = TextureSource::FromUploads;
    auto found = s.decoded_by_key.find(key);
    if (found != s.decoded_by_key.end()) {
        s.counts.decode_hits++;
        s.decoded.splice(s.decoded.begin(), s.decoded, found->second);
        answer.texture = found->second->second;
        return answer;
    }
    answer.texture = s.decode(key);
    s.keep_decoded(key, answer.texture);
    return answer;
}

std::optional<TextureSource> TextureStore::load_clut(uint64_t tex0, uint64_t texclut) {
    State &s = *state_;
    uint32_t psm = field(tex0, 20, 6), cbp = field(tex0, 37, 14), cld = field(tex0, 61, 3);
    if (!is_palette(psm)) {
        return std::nullopt;
    }
    switch (cld) {
    case 1:
        break;
    case 2:
    case 3:
        s.clut_cbp[cld - 2] = cbp;
        break;
    case 4:
    case 5:
        if (s.clut_cbp[cld - 4] == cbp) {
            return std::nullopt;
        }
        s.clut_cbp[cld - 4] = cbp;
        break;
    default:
        return std::nullopt;
    }
    s.counts.clut_loads++;
    Owners owners = s.owners_of(wrapped(clut_rect(tex0, texclut)), 0);
    std::array<uint32_t, 256> values{};
    bool from_uploads = owners.source == TextureSource::FromUploads;
    if (from_uploads) {
        s.sort_by_upload(owners.chunks);
        values = s.clut_from(owners.chunks, tex0, texclut);
    }
    bool ct32 = field(tex0, 51, 4) == 0;
    uint32_t entries = has_256_entries(psm) ? 256 : 16;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t at = clut_halfword(tex0, i);
        s.clut[at] = from_uploads ? uint16_t(values[i]) : s.clut[at];
        s.clut_unloaded[at] = !from_uploads;
        if (ct32) {
            s.clut[at + 256] = from_uploads ? uint16_t(values[i] >> 16) : s.clut[at + 256];
            s.clut_unloaded[at + 256] = !from_uploads;
        }
    }
    return owners.source;
}

ClutColours TextureStore::clut_colours(uint64_t tex0, uint64_t texa) const {
    const State &s = *state_;
    ClutColours out;
    out.from_uploads = true;
    uint32_t entries = has_256_entries(field(tex0, 20, 6)) ? 256 : 16;
    bool ct32 = field(tex0, 51, 4) == 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t at = clut_halfword(tex0, i);
        if (ct32) {
            out.rgba[i] = uint32_t(s.clut[at]) | uint32_t(s.clut[at + 256]) << 16;
            out.from_uploads = out.from_uploads && !s.clut_unloaded[at] && !s.clut_unloaded[at + 256];
            continue;
        }
        uint32_t v = s.clut[at];
        uint32_t r = (v & 0x1F) << 3, g = ((v >> 5) & 0x1F) << 3, b = ((v >> 10) & 0x1F) << 3;
        uint32_t a = (v & 0x8000)                             ? field(texa, 32, 8)
                     : (field(texa, 15, 1) && (v & 0x7FFF) == 0) ? 0
                                                                 : field(texa, 0, 8);
        out.rgba[i] = r | g << 8 | b << 16 | a << 24;
        out.from_uploads = out.from_uploads && !s.clut_unloaded[at];
    }
    return out;
}

std::vector<uint8_t> TextureStore::vram_image() {
    State &s = *state_;
    std::vector<uint64_t> ids;
    ids.reserve(s.chunks.size());
    for (const auto &[id, c] : s.chunks) {
        ids.push_back(id);
    }
    s.sort_by_upload(ids);
    std::fill(s.scratch.begin(), s.scratch.end(), uint8_t(0));
    s.write_chunks(ids, {});
    std::vector<uint8_t> image = s.scratch;
    s.decoded.clear(); // (the scratch is not what any decode left: none is found by it, but say so plainly)
    s.decoded_by_key.clear();
    s.decoded_bytes = 0;
    return image;
}

void TextureStore::restore_vram(std::span<const uint8_t> vram) {
    forget();
    if (vram.size() != GSN_VRAM_BYTES) {
        return;
    }
    const uint64_t whole = uint64_t(16) << 16 | uint64_t(GSN_PSMCT32) << 24; // (BP 0, BW 16, PSMCT32)
    const uint64_t size = 1024 | uint64_t(1024) << 32;
    std::vector<uint8_t> bytes(GSN_VRAM_BYTES);
    gsn_vram_read_image(vram.data(), whole, 0, size, bytes.data(), bytes.size() / 16);
    upload({whole << 32, 0, size}, bytes);
}

TextureSource TextureStore::page_source(uint32_t page) const {
    if (page >= kPages) {
        return TextureSource::NotUploaded;
    }
    return state_->owners_of({page * kBlocksPerPage, 1, GSN_PSMCT32, 0, 0, 64, 32}, 0).source;
}

TextureStore::ClutState TextureStore::clut_state() const {
    const State &s = *state_;
    ClutState c;
    c.clut = s.clut;
    for (size_t i = 0; i < 512; i++) {
        c.unloaded[i] = s.clut_unloaded[i] ? 1 : 0;
    }
    c.cbp = s.clut_cbp;
    return c;
}

void TextureStore::set_clut_state(const ClutState &c) {
    State &s = *state_;
    s.clut = c.clut;
    for (size_t i = 0; i < 512; i++) {
        s.clut_unloaded[i] = c.unloaded[i] != 0;
    }
    s.clut_cbp = c.cbp;
}

void TextureStore::forget() {
    auto fresh = std::make_unique<State>();
    fresh->limits = state_->limits;
    fresh->counts = state_->counts; // (the counts go on: --stats's)
    state_ = std::move(fresh);
}

TextureStoreCounts TextureStore::counts() const {
    const State &s = *state_;
    TextureStoreCounts c = s.counts;
    c.chunks_kept = s.chunks.size();
    c.chunk_bytes_kept = s.chunk_bytes;
    c.decoded_bytes_kept = s.decoded_bytes;
    return c;
}

} // namespace pcr
