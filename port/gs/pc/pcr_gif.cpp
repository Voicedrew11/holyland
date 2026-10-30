// The PC renderer's GIF front end (pcr_gif.h): the GIF's tags, the GS's register writes, its vertex queue and its
// image transfers, as the native GS's front end has them (port/gs/native/gsn.cpp), made into events.
#include "pcr_gif.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <utility>

namespace pcr {

namespace {

uint64_t bits(uint64_t value, unsigned first, unsigned count) {
    return (value >> first) & ((uint64_t(1) << count) - 1);
}

uint64_t read_u64(const uint8_t *at) {
    uint64_t value;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

float as_float(uint64_t low_bits) {
    return std::bit_cast<float>(uint32_t(low_bits));
}

// The pixel formats a transfer can carry (the GS's PSM codes)
constexpr uint32_t psmct32 = 0x00, psmct24 = 0x01, psmct16 = 0x02, psmct16s = 0x0A, psmt8 = 0x13, psmt4 = 0x14,
                   psmt8h = 0x1B, psmt4hl = 0x24, psmt4hh = 0x2C, psmz32 = 0x30, psmz24 = 0x31, psmz16 = 0x32,
                   psmz16s = 0x3A;

// Bits a pixel of the format takes in a transfer; 0 for a PSM the GS does not have
uint32_t psm_bits(uint64_t psm) {
    switch (psm) {
    case psmct32:
    case psmz32:
        return 32;
    case psmct24:
    case psmz24:
        return 24;
    case psmct16:
    case psmct16s:
    case psmz16:
    case psmz16s:
        return 16;
    case psmt8:
    case psmt8h:
        return 8;
    case psmt4:
    case psmt4hl:
    case psmt4hh:
        return 4;
    }
    return 0;
}

bool psm_is_16(uint64_t psm) {
    return psm == psmct16 || psm == psmct16s || psm == psmz16 || psm == psmz16s;
}

// XYZ's X and Y from a PACKED XYZ qword's low half
uint64_t packed_xy(uint64_t low) {
    return bits(low, 0, 16) | bits(low, 32, 16) << 16;
}

// PACKED XYZF: X, Y, 24 bits of Z and F
uint64_t packed_xyzf(uint64_t low, uint64_t high) {
    return packed_xy(low) | bits(high, 4, 24) << 32 | bits(high, 36, 8) << 56;
}

// PACKED XYZ: X, Y and 32 bits of Z
uint64_t packed_xyz(uint64_t low, uint64_t high) {
    return packed_xy(low) | bits(high, 0, 32) << 32;
}

bool packed_adc(uint64_t high) {
    return bits(high, 47, 1) != 0;
}

} // namespace

GifFrontEnd::GifFrontEnd(GifEvents &events) : events_(events) {}

void GifFrontEnd::feed(int path, std::span<const uint8_t> bytes) {
    if (path < 0 || path >= path_count) {
        return;
    }
    PathTag &tag = paths_[size_t(path)];
    feeding_ = true; // (the packets' own writes: their kicks are numbered in a picture between's walk)
    const size_t qwords = bytes.size() / 16;
    size_t i = 0;
    while (i < qwords) {
        const uint8_t *at = bytes.data() + i * 16;
        if (tag.qwords_left == 0) {
            read_tag(tag, read_u64(at), read_u64(at + 8));
            i++;
            continue;
        }
        if (tag.mode == TagMode::image) {
            // as many qwords as there are, at once
            const size_t n = std::min(size_t(tag.qwords_left), qwords - i);
            image_data(bytes.subspan(i * 16, n * 16));
            tag.qwords_left -= uint32_t(n);
            i += n;
            continue;
        }
        if (tag.mode == TagMode::packed) {
            const uint32_t descriptor = uint32_t(bits(tag.registers, tag.next_register * 4, 4));
            tag.next_register = tag.next_register + 1 == tag.register_count ? 0 : tag.next_register + 1;
            write_packed(descriptor, read_u64(at), read_u64(at + 8));
        } else {
            write_reglist(tag, read_u64(at), read_u64(at + 8));
        }
        tag.qwords_left--;
        i++;
    }
    feeding_ = false;
}

void GifFrontEnd::read_tag(PathTag &tag, uint64_t low, uint64_t high) {
    const uint32_t nloop = uint32_t(bits(low, 0, 15));
    const uint32_t flg = uint32_t(bits(low, 58, 2));
    tag.register_count = bits(low, 60, 4) == 0 ? 16 : uint32_t(bits(low, 60, 4));
    tag.registers = high;
    tag.next_register = 0;
    file_.internal_q = 0x3F800000u; // a tag sets Q to 1
    if (flg == 0) {
        tag.mode = TagMode::packed;
        tag.qwords_left = nloop * tag.register_count;
        if (bits(low, 46, 1)) { // PRE: the tag's PRIM written
            write_register(gs_reg::prim, bits(low, 47, 11));
        }
        return;
    }
    if (flg == 1) {
        tag.mode = TagMode::reglist;
        tag.dwords_left = nloop * tag.register_count;
        tag.qwords_left = (tag.dwords_left + 1) / 2;
        return;
    }
    tag.mode = TagMode::image; // (FLG 3 is IMAGE too)
    tag.qwords_left = nloop;
}

void GifFrontEnd::write_packed(uint32_t descriptor, uint64_t low, uint64_t high) {
    switch (descriptor) {
    case 0x0:
        write_register(gs_reg::prim, bits(low, 0, 11));
        return;
    case 0x1: // R, G, B, A from four words, Q from the last ST
        write_register(gs_reg::rgbaq, bits(low, 0, 8) | bits(low, 32, 8) << 8 | bits(high, 0, 8) << 16 |
                                          bits(high, 32, 8) << 24 | uint64_t(file_.internal_q) << 32);
        rgba_q_src_ = q_src_; // (its Q is the last PACKED ST's)
        return;
    case 0x2:
        file_.internal_q = uint32_t(high);
        q_src_ = kicks_;
        write_register(gs_reg::st, low);
        return;
    case 0x3:
        write_register(gs_reg::uv, bits(low, 0, 14) | bits(low, 32, 14) << 16);
        return;
    case 0x4:
        write_register(packed_adc(high) ? gs_reg::xyzf3 : gs_reg::xyzf2, packed_xyzf(low, high));
        return;
    case 0x5:
        write_register(packed_adc(high) ? gs_reg::xyz3 : gs_reg::xyz2, packed_xyz(low, high));
        return;
    case 0xA:
        write_register(gs_reg::fog, bits(high, 36, 8) << 56);
        return;
    case 0xC:
        write_register(gs_reg::xyzf3, packed_xyzf(low, high));
        return;
    case 0xD:
        write_register(gs_reg::xyz3, packed_xyz(low, high));
        return;
    case 0xE: // A+D
        write_register(uint32_t(bits(high, 0, 8)), low);
        return;
    case 0xF: // NOP
        return;
    }
    write_register(descriptor, low); // TEX0, CLAMP (and 0xB): the register's own value
}

void GifFrontEnd::write_reglist(PathTag &tag, uint64_t low, uint64_t high) {
    for (uint64_t value : {low, high}) {
        if (tag.dwords_left == 0) {
            return; // (the last qword's high half, when the count is odd)
        }
        const uint32_t descriptor = uint32_t(bits(tag.registers, tag.next_register * 4, 4));
        tag.next_register = tag.next_register + 1 == tag.register_count ? 0 : tag.next_register + 1;
        tag.dwords_left--;
        if (descriptor < 0xE) { // (A+D and NOP write nothing in REGLIST)
            write_register(descriptor, value);
        }
    }
}

void GifFrontEnd::write_register(uint32_t address, uint64_t value) {
    switch (address) {
    case gs_reg::xyzf2:
    case gs_reg::xyz2:
    case gs_reg::xyzf3:
    case gs_reg::xyz3:
        kick(address, value);
        return;
    case gs_reg::rgbaq:
    case gs_reg::st:
    case gs_reg::uv:
    case gs_reg::fog:
        file_.reg[address] = value; // (no event: the vertices carry them)
        // (in whose stretch it was written: write_packed sets RGBAQ's Q's after)
        if (address == gs_reg::rgbaq) {
            rgba_src_ = kicks_;
            rgba_q_src_ = no_kick;
        } else if (address == gs_reg::st) {
            st_src_ = kicks_;
        } else if (address == gs_reg::uv) {
            uv_src_ = kicks_;
        }
        return;
    case gs_reg::prim:
        write_prim(value);
        return;
    case gs_reg::tex0_1:
    case gs_reg::tex0_2:
        write_tex0(address - gs_reg::tex0_1, value);
        return;
    case gs_reg::tex2_1:
    case gs_reg::tex2_2: {
        // TEX0's PSM and CLUT fields only
        const uint32_t context = address - gs_reg::tex2_1;
        const uint64_t mask = uint64_t(0x3F) << 20 | ~uint64_t(0) << 37;
        write_tex0(context, (file_.reg[gs_reg::tex0_1 + context] & ~mask) | (value & mask));
        return;
    }
    case gs_reg::texflush:
    case 0x0F: // NOP
        return;
    case gs_reg::native_draw: {
        // (a native draw's writes are not the packets': their kicks are not numbered)
        const bool feeding = feeding_;
        feeding_ = false;
        events_.native_draw(value);
        feeding_ = feeding;
        return;
    }
    case gs_reg::upload_again:
        events_.upload_again(value);
        return;
    case gs_reg::frame_mark:
        events_.frame_mark(value);
        return;
    case gs_reg::widescreen:
        events_.widescreen(value);
        return;
    case gs_reg::signal:
    case gs_reg::finish:
    case gs_reg::label:
        events_.gs_sync(address, value);
        return;
    case gs_reg::trxdir:
        store(address, value);
        begin_transfer(uint32_t(value & 3));
        return;
    case gs_reg::hwreg: {
        uint8_t data[8];
        std::memcpy(data, &value, sizeof(data));
        image_data(data);
        return;
    }
    }
    store(address & 0xFF, value);
}

void GifFrontEnd::store(uint32_t address, uint64_t value) {
    const uint64_t old = file_.reg[address];
    if (old == value) {
        return;
    }
    file_.reg[address] = value;
    events_.register_written(address, old, value);
}

void GifFrontEnd::write_prim(uint64_t value) {
    queued_ = 0;
    store(gs_reg::prim, value & 0x7FF);
}

void GifFrontEnd::write_tex0(uint32_t context, uint64_t value) {
    file_.reg[gs_reg::tex0_1 + context] = value;
    make_mip_addresses(context);
    events_.texture_written(context, value, file_.reg[gs_reg::texclut]);
}

// TEX1's MTBA: a TEX0 written makes MIPTBP1 itself, levels 1 to 3 packed after level 0, each a quarter of the one
// before and half its buffer width (gsn.cpp's mip_addresses)
void GifFrontEnd::make_mip_addresses(uint32_t context) {
    if (!bits(file_.reg[gs_reg::tex1_1 + context], 9, 1)) {
        return;
    }
    const uint64_t tex0 = file_.reg[gs_reg::tex0_1 + context];
    const uint64_t psm = bits(tex0, 20, 6);
    const uint32_t width = 1u << bits(tex0, 26, 4), height = 1u << bits(tex0, 30, 4);
    // a block of the format, in pixels
    const uint32_t block_w = psm == psmt4 ? 32 : psm == psmt8 || psm_is_16(psm) ? 16 : 8;
    const uint32_t block_h = psm == psmt8 || psm == psmt4 ? 16 : 8;
    uint32_t base = uint32_t(bits(tex0, 0, 14));
    uint32_t blocks = (width / block_w) * (height / block_h);
    uint32_t row = width / 64;
    uint64_t miptbp1 = 0;
    for (unsigned level = 0; level < 3; level++) {
        base += blocks;
        blocks /= 4;
        row /= 2;
        miptbp1 |= (uint64_t(base & 0x3FFF) | uint64_t(row & 0x3F) << 14) << (level * 20);
    }
    store(gs_reg::miptbp1_1 + context, miptbp1);
}

uint32_t GifFrontEnd::prim_in_effect() const {
    uint32_t prim = uint32_t(file_.reg[gs_reg::prim]) & 0x7FF;
    if (!(file_.reg[gs_reg::prmodecont] & 1)) {
        prim = (prim & 7) | (uint32_t(file_.reg[gs_reg::prmode]) & 0x7F8);
    }
    return prim;
}

GifVertex GifFrontEnd::latch_vertex(uint32_t address, uint64_t value) const {
    const bool with_fog = address == gs_reg::xyzf2 || address == gs_reg::xyzf3;
    GifVertex v;
    v.x = uint16_t(bits(value, 0, 16));
    v.y = uint16_t(bits(value, 16, 16));
    v.z = with_fog ? uint32_t(bits(value, 32, 24)) : uint32_t(value >> 32);
    v.fog = uint8_t(with_fog ? bits(value, 56, 8) : bits(file_.reg[gs_reg::fog], 56, 8));
    const uint64_t rgbaq = file_.reg[gs_reg::rgbaq], st = file_.reg[gs_reg::st], uv = file_.reg[gs_reg::uv];
    v.rgba = uint32_t(rgbaq);
    v.q = as_float(rgbaq >> 32);
    v.s = as_float(st);
    v.t = as_float(st >> 32);
    v.u = uint16_t(bits(uv, 0, 14));
    v.v = uint16_t(bits(uv, 16, 14));
    return v;
}

// A vertex kick: the vertex queued, and the primitive it finishes drawn if the kick draws (XYZ2, XYZF2). Strips
// keep their last two vertices, fans their first and last, line strips their last (gsn.cpp's vertex_kick)
void GifFrontEnd::kick(uint32_t address, uint64_t value) {
    const bool draws = address == gs_reg::xyzf2 || address == gs_reg::xyz2;
    const uint32_t prim = prim_in_effect();
    GifVertex v = latch_vertex(address, value);
    if (numbering_ && feeding_) {
        v.kick = kicks_++;
        v.st_owner = st_src_;
        v.rgba_owner = rgba_src_;
        v.q_owner = rgba_q_src_;
        v.uv_owner = uv_src_;
        events_.kick_numbered(v, prim);
    }
    switch (prim & 7) {
    case 0: // points
        queue_[0] = v;
        queued_ = 0;
        if (draws) {
            emit(PrimitiveShape::point, prim, 1);
        }
        return;
    case 1: // lines
        queue_[queued_++] = v;
        if (queued_ == 2) {
            queued_ = 0;
            if (draws) {
                emit(PrimitiveShape::line, prim, 2);
            }
        }
        return;
    case 2: // a line strip
        if (queued_ == 2) {
            queue_[0] = queue_[1];
            queued_ = 1;
        }
        queue_[queued_++] = v;
        if (queued_ == 2 && draws) {
            emit(PrimitiveShape::line, prim, 2);
        }
        return;
    case 3: // triangles
        queue_[queued_++] = v;
        if (queued_ == 3) {
            queued_ = 0;
            if (draws) {
                emit(PrimitiveShape::triangle, prim, 3);
            }
        }
        return;
    case 4: // a strip
        if (queued_ == 3) {
            queue_[0] = queue_[1];
            queue_[1] = queue_[2];
            queued_ = 2;
        }
        queue_[queued_++] = v;
        if (queued_ == 3 && draws) {
            emit(PrimitiveShape::triangle, prim, 3);
        }
        return;
    case 5: // a fan
        if (queued_ == 3) {
            queue_[1] = queue_[2];
            queued_ = 2;
        }
        queue_[queued_++] = v;
        if (queued_ == 3 && draws) {
            emit(PrimitiveShape::triangle, prim, 3);
        }
        return;
    case 6: // sprites
        queue_[queued_++] = v;
        if (queued_ == 2) {
            queued_ = 0;
            if (draws) {
                emit(PrimitiveShape::sprite, prim, 2);
            }
        }
        return;
    }
    queued_ = 0; // (type 7: the GS has none)
}

void GifFrontEnd::emit(PrimitiveShape shape, uint32_t prim, uint32_t vertex_count) {
    GifPrimitive p;
    p.shape = shape;
    p.prim = prim;
    p.vertex_count = vertex_count;
    std::copy_n(queue_.begin(), vertex_count, p.vertices.begin());
    events_.primitive(p);
}

void GifFrontEnd::number_kicks() {
    numbering_ = true;
    kicks_ = 0;
    st_src_ = rgba_src_ = q_src_ = rgba_q_src_ = uv_src_ = no_kick;
}

void GifFrontEnd::stop_numbering() {
    numbering_ = false;
}

void GifFrontEnd::restore(const GsRegisterFile &file) {
    file_ = file;
    paths_ = {};
    queue_ = {};
    queued_ = 0;
    upload_.reset();
}

void GifFrontEnd::begin_transfer(uint32_t direction) {
    cut_upload_short();
    const TransferRegisters transfer = {file_.reg[gs_reg::bitbltbuf], file_.reg[gs_reg::trxpos],
                                        file_.reg[gs_reg::trxreg]};
    switch (direction) {
    case 0: {
        // host to local: the destination's format, TRXREG's width and height
        const uint64_t width = bits(transfer.trxreg, 0, 12), height = bits(transfer.trxreg, 32, 12);
        const uint64_t pixel_bits = psm_bits(bits(transfer.bitbltbuf, 56, 6));
        const size_t expected = size_t((width * height * pixel_bits + 7) / 8);
        if (expected == 0) {
            return; // (no pixels, or no format: its data is dropped)
        }
        upload_.emplace();
        upload_->transfer = transfer;
        upload_->expected_bytes = expected;
        return;
    }
    case 1:
        events_.local_to_host(transfer);
        return;
    case 2:
        events_.local_copy(transfer);
        return;
    }
}

void GifFrontEnd::cut_upload_short() {
    if (!upload_) {
        return;
    }
    const PendingUpload cut = std::move(*upload_);
    upload_.reset();
    events_.image_upload(cut.transfer, cut.bytes, UploadEnd::cut_short);
}

// A transfer's data (IMAGE or HWREG): kept until the transfer has all its bytes; past them, or with no host-to-
// local transfer begun, dropped
void GifFrontEnd::image_data(std::span<const uint8_t> bytes) {
    if (!upload_) {
        return;
    }
    const size_t wanted = upload_->expected_bytes - upload_->bytes.size();
    const std::span<const uint8_t> taken = bytes.first(std::min(wanted, bytes.size()));
    if (upload_->bytes.empty() && taken.size() == upload_->expected_bytes) {
        // all of it at once: given from the packet, not kept
        const TransferRegisters transfer = upload_->transfer;
        upload_.reset();
        events_.image_upload(transfer, taken, UploadEnd::complete);
        return;
    }
    upload_->bytes.insert(upload_->bytes.end(), taken.begin(), taken.end());
    if (upload_->bytes.size() < upload_->expected_bytes) {
        return;
    }
    const PendingUpload done = std::move(*upload_);
    upload_.reset();
    events_.image_upload(done.transfer, done.bytes, UploadEnd::complete);
}

} // namespace pcr
