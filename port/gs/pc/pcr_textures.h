// The PC renderer's textures from their uploads: what the GS's VRAM is to it, as far as textures go.
//
// Every host-to-local image transfer is kept whole as a chunk (where it went, its bytes), found again by
// its bytes: the same bytes sent to the same place are the same chunk. For each 256-byte block of VRAM
// the store knows which chunks own which nibbles of its 64 words, in the order they came: a later upload
// takes its nibbles from those before it. The formats that own part of a word own only that part
// (PSMCT24 and Z24 the low 24 bits, PSMT8H the top byte, PSMT4HL and HH a nibble of it), and an upload's
// edges own their pixels and no more. For each 8 KB page it knows when a surface was last drawn into it
// or a copy wrote it, for the low 24 bits and the top byte apart.
//
// A texture is the uploads' when every nibble of every level it reads is owned by an upload not drawn
// over since. It is then decoded from those uploads alone (written into a scratch VRAM, oldest first,
// over blocks made zero, and read back with gsn_vram.c's functions), once per texture and set of
// uploads, and found again by its key. Otherwise the answer says why: drawn into (the caller samples its
// surface) or no upload's.
//
// The CLUT buffer is mirrored the same way: TEX0's CLD loads read the CLUT from its uploads.
//
// The design is the native GS's page-ownership table (docs/port/NATIVE_DRAW_P8.md, "Textures from their
// source: the page-ownership table"; port/gs/native/gsn_standalone.inc), without the GS's VRAM behind it.
//
// Everything a TextureStore is given comes from the game's GIF packets and is checked: a rectangle that
// runs off VRAM wraps as the GS wraps it, a format the GS does not have is refused.
//
// One thread at a time (the renderer's GS thread): a TextureStore keeps no state outside itself.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pcr {

// A host-to-local (or local-to-local) transfer's registers as the GIF packet set them
struct TransferRegs {
    uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
};

// A rectangle of a buffer in VRAM: base pointer in 256-byte blocks, width in 64-pixel units, the GS's
// PSM code, and pixels. x and y wrap at 2048 as the GS's do.
struct VramRect {
    uint32_t bp = 0, bw = 0, psm = 0;
    uint32_t x = 0, y = 0, width_px = 0, height_px = 0;
};

// The registers a texture is sampled with (a context's TEX0, TEX1, MIPTBP1, MIPTBP2, CLAMP)
struct TextureRegs {
    uint64_t tex0 = 0, tex1 = 0, miptbp1 = 0, miptbp2 = 0, clamp = 0;
};

enum class TextureSource {
    FromUploads,  // every nibble read is an upload's: decoded from them
    DrawnSurface, // drawn into or copied over since its uploads: the caller samples that surface
    NotUploaded,  // part of it is no upload's (never uploaded, or forgotten)
};

// What a decoded texture is, exactly: its shape, the part of it read, and the uploads (chunk ids, never
// reused) it was written from, oldest first. Two equal keys are the same texels.
struct TextureKey {
    uint32_t tbp = 0, tbw = 0, psm = 0, tw_log2 = 0, th_log2 = 0, levels = 0;
    std::array<uint32_t, 7> level_tbp{}, level_tbw{};
    uint32_t read_cols = 0, read_rows = 0;
    std::vector<uint64_t> chunks;

    bool operator==(const TextureKey &) const = default;
    uint64_t hash() const;
};

enum class TexelKind {
    Rgba8,  // PSMCT32/24/16/16S and the Z formats: rgba
    Index8, // PSMT8, T4, T8H, T4HL, T4HH: CLUT indices (0-15 for the 4-bit formats)
};

// A texture's texels, every level of it back to back, each level's rows top down.
// Rgba8 is R | G << 8 | B << 16 | A << 24, raw as VRAM has it, TEXA left to the shader:
//   PSMCT32/Z32: the word as it is (A 0x80 is opaque to the GS).
//   PSMCT24/Z24: the low 24 bits, A 0 (the shader gives TEXA's TA0, or 0 where AEM and the colour is black).
//   PSMCT16/16S/Z16/Z16S: each 5 bits shifted up 3 (as the GS expands them), A 0xFF where bit 15 is set
//   (the shader gives TA1) and 0 where it is not (TA0, or 0 where AEM and the colour is black).
struct DecodedTexture {
    TexelKind kind = TexelKind::Rgba8;
    uint32_t width_px = 0, height_px = 0, levels = 0;
    // the part of level 0 that is the uploads' (level l's is these >> l, at least 1); the texels past it
    // are what those uploads put there over zero
    uint32_t read_cols = 0, read_rows = 0;
    std::array<size_t, 7> level_start{}; // each level's first texel
    std::vector<uint32_t> rgba;          // (Rgba8)
    std::vector<uint8_t> indices;        // (Index8)

    uint32_t level_width(uint32_t level) const;
    uint32_t level_height(uint32_t level) const;
};

// Where a texture or a CLUT is not the uploads': the level and the block (0..16383) found first;
// block is UINT32_MAX when TEX0 has a format the GS does not have
struct MissingBlock {
    uint32_t level = 0, block = 0;
};

struct TextureAnswer {
    TextureSource source = TextureSource::NotUploaded;
    TextureKey key;                                // (FromUploads)
    std::shared_ptr<const DecodedTexture> texture; // (FromUploads)
    MissingBlock missing;                          // (DrawnSurface, NotUploaded)
};

// The 256 colours a palette texture's indices look up (16 for the 4-bit formats, the rest 0), as RGBA8
// with TEXA applied to the 16-bit CLUT formats, as the GS's CLUT buffer has them now
struct ClutColours {
    std::array<uint32_t, 256> rgba{};
    bool from_uploads = false; // false: an entry read was not loaded from uploads (the caller reads its own)
};

struct TextureStoreLimits {
    size_t decoded_bytes = size_t(256) << 20; // decoded textures kept (least recently used let go)
    size_t unowned_chunk_bytes = size_t(64) << 20; // chunks that own nothing now, kept to be found again
};

struct TextureStoreCounts {
    uint64_t uploads = 0, upload_bytes = 0, uploads_refused = 0;
    uint64_t chunks_new = 0, chunks_kept = 0, chunk_bytes_kept = 0;
    uint64_t resolves = 0, decodes = 0, decode_hits = 0, decoded_bytes_kept = 0;
    uint64_t clut_loads = 0, clut_decodes = 0, clut_hits = 0;
    uint64_t copies_as_uploads = 0; // local copies whose source was uploads': uploads of what they wrote
    uint64_t read_backs = 0, read_backs_refused = 0;
    uint64_t uploads_again = 0; // (of uploads: made again from their chunk)
};

class TextureStore {
public:
    explicit TextureStore(TextureStoreLimits limits = {});
    ~TextureStore();
    TextureStore(TextureStore &&) noexcept;
    TextureStore &operator=(TextureStore &&) noexcept;
    TextureStore(const TextureStore &) = delete;
    TextureStore &operator=(const TextureStore &) = delete;

    // A host-to-local transfer, whole: the bytes its IMAGE data carried (past the rectangle ignored; fewer
    // than it needs: the pixels they hold). false when refused (a format the GS does not have, no pixels).
    bool upload(const TransferRegs &regs, std::span<const uint8_t> bytes);
    // The chunk the last upload taken was kept as (0: none yet), and that upload made again from it (the same bytes to
    // the same place, so nothing compared: the pictures between ask for the frame's uploads again). False if the
    // chunk has been let go (the caller sends the bytes again).
    uint64_t last_chunk() const;
    bool upload_again(uint64_t chunk);

    // A surface drawn into over this rectangle (in its format: a 24-bit one leaves the top byte the uploads')
    void drawn(const VramRect &rect);
    // A local-to-local transfer: when every pixel it reads is the uploads', an upload of what it writes (made
    // as the GS makes the copy); else its destination drawn over
    void copied(const TransferRegs &regs);

    // A local-to-host transfer (TRXDIR 1): when every pixel it reads is the uploads', its pixels as the GS sends
    // them (packed as for host-to-local, zeroes past the last pixel) into out, which is resized to whole qwords;
    // false (out left empty) when part of it is no upload's or was drawn over
    bool read_back(const TransferRegs &regs, std::vector<uint8_t> &out);

    // The texture a draw samples. cols_read and rows_read: the texels of level 0 its draws read across and
    // down (1 << tw and 1 << th when not known); without mipmaps and not REGION_REPEAT, the texture is
    // only that part, rounded up to whole pages, as the native GS has it.
    TextureAnswer resolve(const TextureRegs &regs, uint32_t cols_read, uint32_t rows_read);

    // TEX0 written: its CLD's load of the CLUT buffer (TEXCLUT for CSM2). nullopt when CLD loads nothing;
    // else where the entries came from (those not FromUploads are marked in the mirror)
    std::optional<TextureSource> load_clut(uint64_t tex0, uint64_t texclut);
    // The colours a texture with this TEX0 sees, from the mirror
    ClutColours clut_colours(uint64_t tex0, uint64_t texa) const;

    // The GS's 4 MB as the uploads made it (each kept chunk written in the order it came, over zeroes): a save
    // state's VRAM, which the caller completes with what its surfaces drew
    std::vector<uint8_t> vram_image();
    // A state's (or another renderer's) VRAM taken: everything forgotten, then the 4 MB as one upload (the whole
    // memory read as 1024 x 1024 PSMCT32 at buffer width 16, which covers every byte once)
    void restore_vram(std::span<const uint8_t> vram);

    // An 8 KB page as a whole: the uploads' (FromUploads), drawn over since (DrawnSurface), or neither
    TextureSource page_source(uint32_t page) const;

    // The CLUT buffer's mirror, for save states
    struct ClutState {
        std::array<uint16_t, 512> clut{};
        std::array<uint8_t, 512> unloaded{}; // 1: not loaded from uploads
        std::array<uint32_t, 2> cbp{};
    };
    ClutState clut_state() const;
    void set_clut_state(const ClutState &state);

    // A state loaded: nothing of VRAM is known any more (chunks, decodes and the CLUT forgotten; the counts
    // go on)
    void forget();

    TextureStoreCounts counts() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace pcr
