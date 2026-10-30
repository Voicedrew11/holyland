// The PC renderer's GIF front end (port/gs/pc/pcr_gif.h): hand-built packets with the events worked out by hand,
// the same packets split at every qword, and random packet streams on three paths, fed in random pieces and
// interleaved, against a reference walk written here from the packets' structure (gsn.cpp's front end cannot be
// linked alone). Then random bytes, for no crash and no read past the data (run it under ASan and UBSan).
#include "pcr_gif.h"

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <optional>
#include <random>
#include <string>
#include <vector>

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

uint32_t float_bits(float f) {
    return std::bit_cast<uint32_t>(f);
}

std::string hex(uint64_t v) {
    char s[24];
    std::snprintf(s, sizeof(s), "%" PRIx64, v);
    return s;
}

uint64_t fnv(std::span<const uint8_t> bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes) {
        h = (h ^ b) * 0x100000001b3ull;
    }
    return h;
}

// ---- Events as text: the front end's and the reference's in one form -------------------------------------------

std::string vertex_text(const GifVertex &v) {
    return "(" + hex(v.x) + "," + hex(v.y) + "," + hex(v.z) + "," + hex(v.rgba) + "," + hex(float_bits(v.q)) + "," +
           hex(float_bits(v.s)) + "," + hex(float_bits(v.t)) + "," + hex(v.u) + "," + hex(v.v) + "," + hex(v.fog) +
           ")";
}

// (with the TEX0 of the primitive's context, read from the register file at the event)
std::string primitive_text(const GifPrimitive &p, uint64_t tex0) {
    std::string s = "prim " + hex(uint32_t(p.shape)) + " " + hex(p.prim) + " n" + hex(p.vertex_count);
    for (uint32_t i = 0; i < p.vertex_count; i++) {
        s += " " + vertex_text(p.vertices[i]);
    }
    return s + " tex0 " + hex(tex0);
}

std::string transfer_text(const TransferRegisters &t) {
    return hex(t.bitbltbuf) + " " + hex(t.trxpos) + " " + hex(t.trxreg);
}

std::string upload_text(const TransferRegisters &t, std::span<const uint8_t> bytes, UploadEnd end) {
    return "upload " + transfer_text(t) + (end == UploadEnd::complete ? " complete " : " cut ") + hex(bytes.size()) +
           " " + hex(fnv(bytes));
}

class Recorder : public GifEvents {
public:
    std::vector<std::string> log;
    std::vector<GifPrimitive> primitives;
    std::vector<std::vector<uint8_t>> uploads;
    const GifFrontEnd *front_end = nullptr;

    void register_written(uint32_t address, uint64_t old_value, uint64_t new_value) override {
        log.push_back("reg " + hex(address) + " " + hex(old_value) + " " + hex(new_value));
    }
    void texture_written(uint32_t context, uint64_t tex0, uint64_t texclut) override {
        log.push_back("tex " + hex(context) + " " + hex(tex0) + " " + hex(texclut));
    }
    void primitive(const GifPrimitive &p) override {
        primitives.push_back(p);
        log.push_back(primitive_text(p, front_end->registers().reg[gs_reg::tex0_1 + ((p.prim >> 9) & 1)]));
    }
    void image_upload(const TransferRegisters &t, std::span<const uint8_t> bytes, UploadEnd end) override {
        uploads.emplace_back(bytes.begin(), bytes.end());
        log.push_back(upload_text(t, bytes, end));
    }
    void local_copy(const TransferRegisters &t) override { log.push_back("copy " + transfer_text(t)); }
    void local_to_host(const TransferRegisters &t) override { log.push_back("read " + transfer_text(t)); }
    void native_draw(uint64_t v) override { log.push_back("native " + hex(v)); }
    void upload_again(uint64_t v) override { log.push_back("again " + hex(v)); }
    void frame_mark(uint64_t v) override { log.push_back("mark " + hex(v)); }
    void widescreen(uint64_t v) override { log.push_back("wide " + hex(v)); }
    void gs_sync(uint32_t address, uint64_t v) override { log.push_back("sync " + hex(address) + " " + hex(v)); }
    std::vector<GifVertex> numbered;
    void kick_numbered(const GifVertex &v, uint32_t) override { numbered.push_back(v); }
};

// A front end with its recorder
struct Rig {
    Recorder events;
    GifFrontEnd gif{events};
    Rig() { events.front_end = &gif; }
    void feed(int path, const std::vector<uint8_t> &bytes) { gif.feed(path, bytes); }
};

// ---- Packets by hand ---------------------------------------------------------------------------------------------

enum : uint32_t { packed = 0, reglist = 1, image = 2 };

struct Packet {
    std::vector<uint8_t> bytes;

    void qword(uint64_t low, uint64_t high) {
        uint8_t q[16];
        std::memcpy(q, &low, 8);
        std::memcpy(q + 8, &high, 8);
        bytes.insert(bytes.end(), q, q + 16);
    }
    // a GIF tag: NREG registers of REGS (4 bits each), NLOOP times; PRE writes PRIM
    void tag(uint32_t nloop, uint32_t flg, uint32_t nreg, uint64_t regs, std::optional<uint32_t> prim = std::nullopt,
             bool eop = false) {
        uint64_t low = uint64_t(nloop & 0x7FFF) | uint64_t(eop) << 15 | uint64_t(flg) << 58 | uint64_t(nreg & 15) << 60;
        if (prim) {
            low |= uint64_t(1) << 46 | uint64_t(*prim & 0x7FF) << 47;
        }
        qword(low, regs);
    }
    void ad(uint32_t address, uint64_t value) { qword(value, address); }
    // a whole A+D packet of these writes
    static Packet writes(std::initializer_list<std::pair<uint32_t, uint64_t>> list) {
        Packet p;
        p.tag(uint32_t(list.size()), packed, 1, 0xE, std::nullopt, true);
        for (auto [a, v] : list) {
            p.ad(a, v);
        }
        return p;
    }
};

uint64_t xyz(uint32_t x, uint32_t y, uint32_t z) {
    return uint64_t(x) | uint64_t(y) << 16 | uint64_t(z) << 32;
}

// PACKED XYZ2 (descriptor 5): X, Y, Z, and ADC
void packed_xyz(Packet &p, uint32_t x, uint32_t y, uint32_t z, bool adc = false) {
    p.qword(uint64_t(x) | uint64_t(y) << 32, uint64_t(z) | uint64_t(adc) << 47);
}

// ---- The hand-built cases ----------------------------------------------------------------------------------------

void test_register_writes() {
    Rig r;
    r.feed(1, Packet::writes({{gs_reg::frame_1, 0x1234}, {gs_reg::frame_1, 0x1234}, {gs_reg::signal, 5},
                              {gs_reg::native_draw, 7}, {gs_reg::upload_again, 8}, {gs_reg::frame_mark, 9},
                              {gs_reg::widescreen, 10}, {gs_reg::finish, 1}, {gs_reg::label, 2},
                              {gs_reg::texflush, 0}, {gs_reg::rgbaq, 0xAB}})
                  .bytes);
    std::vector<std::string> want = {"reg 4c 0 1234", "sync 60 5", "native 7", "again 8", "mark 9", "wide a",
                                     "sync 61 1", "sync 62 2"};
    CHECK(r.events.log == want);
    CHECK(r.gif.registers().reg[gs_reg::frame_1] == 0x1234);
    CHECK(r.gif.registers().reg[gs_reg::signal] == 0); // (not kept, as gsn's)
    CHECK(r.gif.registers().reg[gs_reg::rgbaq] == 0xAB);
}

// One kind of PRIM, kicked `count` times with vertices 0, 1, 2 ... (X = 16 * i); the vertices' X / 16 of each
// primitive drawn
std::vector<std::vector<uint32_t>> kicks(uint32_t prim, uint32_t count, uint32_t adc_mask = 0) {
    Rig r;
    Packet p;
    p.tag(count, packed, 1, 0x5, prim, true);
    for (uint32_t i = 0; i < count; i++) {
        packed_xyz(p, i * 16, 0, 0, (adc_mask >> i) & 1);
    }
    r.feed(1, p.bytes);
    std::vector<std::vector<uint32_t>> drawn;
    for (const GifPrimitive &g : r.events.primitives) {
        std::vector<uint32_t> xs;
        for (uint32_t i = 0; i < g.vertex_count; i++) {
            xs.push_back(g.vertices[i].x / 16u);
        }
        drawn.push_back(xs);
    }
    return drawn;
}

void test_primitive_types() {
    using V = std::vector<std::vector<uint32_t>>;
    CHECK(kicks(0, 3) == (V{{0}, {1}, {2}}));
    CHECK(kicks(1, 5) == (V{{0, 1}, {2, 3}}));
    CHECK(kicks(2, 4) == (V{{0, 1}, {1, 2}, {2, 3}}));
    CHECK(kicks(3, 7) == (V{{0, 1, 2}, {3, 4, 5}}));
    CHECK(kicks(4, 5) == (V{{0, 1, 2}, {1, 2, 3}, {2, 3, 4}}));
    CHECK(kicks(5, 5) == (V{{0, 1, 2}, {0, 2, 3}, {0, 3, 4}}));
    CHECK(kicks(6, 5) == (V{{0, 1}, {2, 3}}));
    CHECK(kicks(7, 6).empty());
    // ADC: the vertex moves the queue on, the primitive it finishes is not drawn
    CHECK(kicks(4, 5, 1u << 2) == (V{{1, 2, 3}, {2, 3, 4}}));
    CHECK(kicks(5, 5, 1u << 3) == (V{{0, 1, 2}, {0, 3, 4}}));
    CHECK(kicks(3, 6, 1u << 5) == (V{{0, 1, 2}}));
    CHECK(kicks(6, 4, 1u << 1) == (V{{2, 3}}));
    CHECK(kicks(2, 3, 1u << 1) == (V{{1, 2}}));
    CHECK(kicks(0, 2, 1u << 0) == (V{{1}}));
    // the shape
    Rig r;
    r.feed(2, Packet::writes({{gs_reg::prmodecont, 1}}).bytes); // (0 as the file starts, as gsn's)
    Packet p;
    p.tag(2, packed, 1, 0x5, 6 | 1u << 9);
    packed_xyz(p, 1, 2, 3);
    packed_xyz(p, 4, 5, 6);
    r.feed(2, p.bytes);
    CHECK(r.events.primitives.size() == 1 && r.events.primitives[0].shape == PrimitiveShape::sprite &&
          r.events.primitives[0].prim == (6 | 1u << 9));
}

void test_xyz2_and_xyz3() {
    // A+D kicks: XYZ3 queues without drawing, XYZ2 draws; XYZF's fog and 24-bit Z
    Rig r;
    r.feed(1, Packet::writes({{gs_reg::prim, 3},
                              {gs_reg::fog, uint64_t(0x55) << 56},
                              {gs_reg::xyz3, xyz(16, 0, 0xFFFFFFFF)},
                              {gs_reg::xyzf3, xyz(32, 0, 0x12345678) | uint64_t(0x9A) << 56},
                              {gs_reg::xyz3, xyz(48, 0, 1)},
                              {gs_reg::xyz3, xyz(64, 0, 1)},
                              {gs_reg::xyz2, xyz(80, 0, 1)},
                              {gs_reg::xyzf2, xyz(96, 0, 1)}})
                  .bytes);
    CHECK(r.events.primitives.size() == 1);
    if (r.events.primitives.size() == 1) {
        const GifPrimitive &g = r.events.primitives[0];
        CHECK(g.vertices[0].x == 64 && g.vertices[2].x == 96);
        CHECK(g.vertices[2].fog == 0 && g.vertices[1].fog == 0x55);
    }
    // the XYZF vertex: Z of 24 bits, F its own
    Rig f;
    f.feed(1, Packet::writes({{gs_reg::prim, 0}, {gs_reg::xyzf2, xyz(1, 2, 0x12345678) | uint64_t(0x9A) << 56},
                              {gs_reg::xyz2, xyz(1, 2, 0xFFFFFFFF)}})
                  .bytes);
    CHECK(f.events.primitives.size() == 2);
    if (f.events.primitives.size() == 2) {
        CHECK(f.events.primitives[0].vertices[0].z == 0x345678 && f.events.primitives[0].vertices[0].fog == 0x9A);
        CHECK(f.events.primitives[1].vertices[0].z == 0xFFFFFFFF);
    }
}

void test_prim_resets_queue() {
    Rig r;
    Packet p = Packet::writes({{gs_reg::prim, 3}, {gs_reg::xyz2, xyz(16, 0, 0)}, {gs_reg::xyz2, xyz(32, 0, 0)},
                               {gs_reg::prim, 3}, {gs_reg::xyz2, xyz(48, 0, 0)}, {gs_reg::xyz2, xyz(64, 0, 0)},
                               {gs_reg::xyz2, xyz(80, 0, 0)}});
    r.feed(1, p.bytes);
    CHECK(r.events.primitives.size() == 1 && r.events.primitives[0].vertices[0].x == 48);
    // the same PRIM again makes no register event, but still empties the queue
    CHECK(r.events.log.size() == 2 && r.events.log[0] == "reg 0 0 3");
}

void test_prmode() {
    Rig r;
    // AC 0: the attributes from PRMODE, the type from PRIM
    r.feed(1, Packet::writes({{gs_reg::prmode, 0x7F8}, {gs_reg::prim, 0x010}, {gs_reg::xyz2, 0}}).bytes);
    CHECK(r.events.primitives.size() == 1 && r.events.primitives[0].prim == 0x7F8);
    r.feed(1, Packet::writes({{gs_reg::prmodecont, 1}, {gs_reg::xyz2, 0}}).bytes);
    CHECK(r.events.primitives.size() == 2 && r.events.primitives[1].prim == 0x010);
}

void test_q_latching() {
    Rig r;
    Packet p;
    const uint32_t q = float_bits(0.25f), s = float_bits(2.0f), t = float_bits(3.0f);
    // points: a kick before any RGBAQ (RGBAQ's Q 0, as the file starts); an RGBAQ before any ST in the tag (Q
    // 1.0); ST then RGBAQ (the ST's Q)
    p.tag(1, packed, 6, 0x512515, 0);
    packed_xyz(p, 0, 0, 0);
    p.qword(0x11 | uint64_t(0x22) << 32, 0x33 | uint64_t(0x44) << 32); // RGBAQ
    packed_xyz(p, 0, 0, 0);
    p.qword(s | uint64_t(t) << 32, q);                                   // ST
    p.qword(0x55 | uint64_t(0x66) << 32, 0x77 | uint64_t(0x88) << 32); // RGBAQ
    packed_xyz(p, 0, 0, 0);
    // a new tag: Q back to 1 for the next RGBAQ
    p.tag(1, packed, 2, 0x51);
    p.qword(1, 2);
    packed_xyz(p, 0, 0, 0);
    r.feed(1, p.bytes);
    CHECK(r.events.primitives.size() == 4);
    if (r.events.primitives.size() == 4) {
        const auto &g = r.events.primitives;
        CHECK(g[0].vertices[0].q == 0.0f && g[0].vertices[0].rgba == 0);
        CHECK(g[1].vertices[0].q == 1.0f && g[1].vertices[0].rgba == 0x44332211u);
        CHECK(g[2].vertices[0].q == 0.25f && g[2].vertices[0].s == 2.0f && g[2].vertices[0].t == 3.0f);
        CHECK(g[2].vertices[0].rgba == 0x88776655u);
        CHECK(g[3].vertices[0].q == 1.0f && g[3].vertices[0].rgba == 0x00020001u);
    }
    CHECK(r.gif.registers().internal_q == 0x3F800000u);
    // A+D RGBAQ keeps its own Q
    r.feed(1, Packet::writes({{gs_reg::rgbaq, uint64_t(q) << 32 | 0x01020304}, {gs_reg::xyz2, 0}}).bytes);
    CHECK(r.events.primitives.size() == 5 && r.events.primitives.back().vertices[0].q == 0.25f);
}

void test_uv_and_fog_packed() {
    Rig r;
    Packet p;
    p.tag(1, packed, 4, 0x4A30, 0);
    p.qword(0, 0);                                // PRIM 0
    p.qword(0x7FFF | uint64_t(0x3ABC) << 32, 0); // UV (14 bits each)
    p.qword(0, 0x9AULL << 36);                    // FOG
    p.qword(5 | uint64_t(6) << 32, 0x777ULL << 4 | 0x12ULL << 36); // XYZF2
    r.feed(1, p.bytes);
    CHECK(r.events.primitives.size() == 1);
    if (r.events.primitives.size() == 1) {
        const GifVertex &v = r.events.primitives[0].vertices[0];
        CHECK(v.u == 0x3FFF && v.v == 0x3ABC && v.x == 5 && v.y == 6 && v.z == 0x777 && v.fog == 0x12);
    }
    CHECK(r.gif.registers().reg[gs_reg::fog] == 0x9AULL << 56);
}

// A picture between's walk (number_kicks): every kick fed numbered, PACKED, A+D or REGLIST, XYZ3 too; the stretch each
// vertex register was written in; a native draw's own kicks not numbered; none after stop_numbering
void test_kick_numbers() {
    // (a native draw's handler that kicks a vertex itself, as the trail's does)
    struct NativeKicks : Recorder {
        GifFrontEnd *gif = nullptr;
        void native_draw(uint64_t) override { gif->write(gs_reg::xyz2, 0); }
    };
    NativeKicks events;
    GifFrontEnd gif(events);
    events.front_end = &gif;
    events.gif = &gif;
    gif.feed(1, Packet::writes({{gs_reg::prim, 6}, {gs_reg::xyz2, 0}}).bytes);
    CHECK(events.numbered.empty() && events.primitives.empty());

    gif.number_kicks();
    Packet p;
    // PACKED: ST, RGBAQ, XYZ2 (kick 0), XYZ3 by ADC (kick 1)
    p.tag(1, packed, 4, 0x5512, 0);
    p.qword(float_bits(1.0f) | uint64_t(float_bits(2.0f)) << 32, float_bits(0.5f)); // ST
    p.qword(1 | uint64_t(2) << 32, 3 | uint64_t(4) << 32);                             // RGBAQ: Q the ST's
    packed_xyz(p, 0x10, 0x20, 0x30);
    packed_xyz(p, 0x11, 0x21, 0x31, true);
    gif.feed(1, p.bytes);
    // A+D: RGBAQ (its own Q), a native draw (its kick not numbered), XYZ2 (kick 2)
    gif.feed(1, Packet::writes({{gs_reg::rgbaq, 5}, {gs_reg::native_draw, 1}, {gs_reg::xyz2, 0}}).bytes);
    // REGLIST: UV, XYZF2 (kick 3)
    Packet r;
    r.tag(1, reglist, 2, 0x43);
    r.qword(0x12 | uint64_t(0x34) << 16, 0);
    gif.feed(2, r.bytes);
    CHECK(gif.kicks_numbered() == 4);
    CHECK(events.numbered.size() == 4);
    if (events.numbered.size() == 4) {
        const auto &n = events.numbered;
        CHECK(n[0].kick == 0 && n[0].st_owner == 0 && n[0].rgba_owner == 0 && n[0].q_owner == 0 && n[0].uv_owner == no_kick);
        CHECK(n[1].kick == 1 && n[1].st_owner == 0 && n[1].rgba_owner == 0 && n[1].q_owner == 0);
        CHECK(n[2].kick == 2 && n[2].st_owner == 0 && n[2].rgba_owner == 2 && n[2].q_owner == no_kick);
        CHECK(n[3].kick == 3 && n[3].uv_owner == 3 && n[3].rgba_owner == 2 && n[3].u == 0x12 && n[3].v == 0x34);
    }
    // the primitives carry their vertices' numbers; the native draw's kick has none
    bool native_unnumbered = false;
    for (const GifPrimitive &g : events.primitives) {
        for (uint32_t i = 0; i < g.vertex_count; i++) {
            native_unnumbered |= g.vertices[i].kick == no_kick;
        }
    }
    CHECK(native_unnumbered);

    gif.stop_numbering();
    gif.feed(1, Packet::writes({{gs_reg::xyz2, 0}}).bytes);
    CHECK(events.numbered.size() == 4 && gif.kicks_numbered() == 4);
    // a new walk numbers from 0, the stretches forgotten
    gif.number_kicks();
    gif.feed(1, Packet::writes({{gs_reg::xyz2, 0}}).bytes);
    CHECK(events.numbered.size() == 5 && events.numbered.back().kick == 0 && events.numbered.back().rgba_owner == no_kick);
}

void test_tex2_merge() {
    Rig r;
    const uint64_t tex0 = 0x123456789ABCDEF0ull;
    const uint64_t tex2 = 0xFEDCBA9876543210ull;
    r.feed(1, Packet::writes({{gs_reg::texclut, 0x77}, {gs_reg::tex0_2, tex0}, {gs_reg::tex2_2, tex2}}).bytes);
    const uint64_t mask = uint64_t(0x3F) << 20 | ~uint64_t(0) << 37;
    const uint64_t merged = (tex0 & ~mask) | (tex2 & mask);
    std::vector<std::string> want = {"reg 1c 0 77", "tex 1 " + hex(tex0) + " 77", "tex 1 " + hex(merged) + " 77"};
    CHECK(r.events.log == want);
    CHECK(r.gif.registers().reg[gs_reg::tex0_2] == merged);
    // the same TEX0 again: its event again (the CLUT is loaded each time)
    r.feed(1, Packet::writes({{gs_reg::tex0_2, merged}}).bytes);
    CHECK(r.events.log.size() == 4 && r.events.log[3] == want[2]);
    // TEX0 by its PACKED descriptor (6, 7)
    Packet p;
    p.tag(1, packed, 1, 0x6);
    p.qword(42, 0xFFFF);
    r.feed(1, p.bytes);
    CHECK(r.gif.registers().reg[gs_reg::tex0_1] == 42);
}

void test_mip_addresses() {
    Rig r;
    // PSMCT32 256 x 256 at block 0: level 0 is 32 x 32 blocks of 8 x 8
    const uint64_t tex0 = uint64_t(8) << 26 | uint64_t(8) << 30;
    r.feed(1, Packet::writes({{gs_reg::tex1_1, 1u << 9}, {gs_reg::tex0_1, tex0}}).bytes);
    const uint64_t want = uint64_t(1024 | 2 << 14) | uint64_t(1280 | 1 << 14) << 20 | uint64_t(1344 | 0 << 14) << 40;
    CHECK(r.gif.registers().reg[gs_reg::miptbp1_1] == want);
}

void test_reglist() {
    Rig r;
    Packet p;
    // 3 registers once: two qwords, the last half unused; A+D and NOP write nothing
    p.tag(1, reglist, 3, 0xC | 0xE << 4 | 0xF << 8); // XYZF3, A+D, NOP
    p.qword(0x55, 0x66);
    p.qword(0x77, 0xDEAD);
    p.tag(1, reglist, 2, 0x10); // PRIM, RGBAQ
    p.qword(3, 0xFF0000FF);
    r.feed(1, p.bytes);
    CHECK(r.gif.registers().reg[gs_reg::prim] == 3 && r.gif.registers().reg[gs_reg::rgbaq] == 0xFF0000FF);
    CHECK(r.gif.registers().reg[0x0E] == 0 && r.gif.registers().reg[0x0F] == 0);
    // NREG 0 is 16
    Rig s;
    Packet q;
    q.tag(1, reglist, 0, 0x1111111111111111ull);
    for (int i = 0; i < 8; i++) {
        q.qword(uint64_t(i * 2), uint64_t(i * 2 + 1));
    }
    q.tag(1, reglist, 1, 0x1);
    q.qword(99, 0);
    s.feed(1, q.bytes);
    CHECK(s.gif.registers().reg[gs_reg::rgbaq] == 99);
}

uint64_t bitbltbuf(uint32_t dbp, uint32_t dbw, uint32_t dpsm) {
    return uint64_t(dbp) << 32 | uint64_t(dbw) << 48 | uint64_t(dpsm) << 56;
}

uint64_t trxreg(uint32_t w, uint32_t h) {
    return uint64_t(w) | uint64_t(h) << 32;
}

Packet image_packet(uint32_t qwords, uint8_t first) {
    Packet p;
    p.tag(qwords, image, 0, 0, std::nullopt, true);
    for (uint32_t i = 0; i < qwords * 16; i++) {
        p.bytes.push_back(uint8_t(first + i));
    }
    return p;
}

void test_uploads() {
    const TransferRegisters t = {bitbltbuf(100, 1, 0), uint64_t(3) << 32, trxreg(4, 2)};
    // IMAGE over two packets, another path's packet between
    {
        Rig r;
        r.feed(2, Packet::writes({{gs_reg::bitbltbuf, t.bitbltbuf}, {gs_reg::trxpos, t.trxpos},
                                  {gs_reg::trxreg, t.trxreg}, {gs_reg::trxdir, 0}})
                      .bytes);
        r.events.log.clear();
        r.feed(2, image_packet(1, 0).bytes);
        CHECK(r.events.uploads.empty());
        r.feed(1, Packet::writes({{gs_reg::frame_1, 1}}).bytes);
        r.feed(2, image_packet(2, 16).bytes); // (one qword too many: dropped)
        CHECK(r.events.uploads.size() == 1);
        if (r.events.uploads.size() == 1) {
            std::vector<uint8_t> want(32);
            for (size_t i = 0; i < 32; i++) {
                want[i] = uint8_t(i);
            }
            CHECK(r.events.uploads[0] == want);
            CHECK(r.events.log.back() == upload_text(t, want, UploadEnd::complete));
        }
    }
    // HWREG: 8 bytes a write; 4 x 1 of PSMCT32 is two
    {
        Rig r;
        r.feed(1, Packet::writes({{gs_reg::bitbltbuf, bitbltbuf(0, 1, 0)}, {gs_reg::trxreg, trxreg(4, 1)},
                                  {gs_reg::trxdir, 0}, {gs_reg::hwreg, 0x0706050403020100ull},
                                  {gs_reg::hwreg, 0x0F0E0D0C0B0A0908ull}, {gs_reg::hwreg, 0xFFFF}})
                      .bytes);
        CHECK(r.events.uploads.size() == 1 && r.events.uploads[0].size() == 16 && r.events.uploads[0][15] == 15);
    }
    // PSMT4, 3 x 1: two bytes from the qword
    {
        Rig r;
        r.feed(1, Packet::writes({{gs_reg::bitbltbuf, bitbltbuf(0, 1, 0x14)}, {gs_reg::trxreg, trxreg(3, 1)},
                                  {gs_reg::trxdir, 0}})
                      .bytes);
        r.feed(1, image_packet(1, 0xA0).bytes);
        CHECK(r.events.uploads.size() == 1 && r.events.uploads[0] == (std::vector<uint8_t>{0xA0, 0xA1}));
    }
    // cut short by a new TRXDIR; an absurd size; no pixels; no format; the copies
    {
        Rig r;
        const TransferRegisters huge = {bitbltbuf(0, 64, 0), 0, trxreg(4095, 4095)};
        r.feed(1, Packet::writes({{gs_reg::bitbltbuf, huge.bitbltbuf}, {gs_reg::trxreg, huge.trxreg},
                                  {gs_reg::trxdir, 0}})
                      .bytes);
        r.feed(1, image_packet(3, 0).bytes);
        r.events.log.clear();
        r.feed(1, Packet::writes({{gs_reg::trxdir, 2}}).bytes);
        CHECK(r.events.uploads.size() == 1 && r.events.uploads[0].size() == 48);
        CHECK(r.events.log.size() == 3 && r.events.log[1] == upload_text(huge, r.events.uploads[0], UploadEnd::cut_short) &&
              r.events.log[2] == "copy " + transfer_text(huge));
        r.events.log.clear();
        r.feed(1, Packet::writes({{gs_reg::trxdir, 1}, {gs_reg::trxreg, trxreg(0, 4)}, {gs_reg::trxdir, 0}}).bytes);
        r.feed(1, image_packet(1, 0).bytes);
        r.feed(1, Packet::writes({{gs_reg::trxreg, trxreg(4, 4)}, {gs_reg::bitbltbuf, bitbltbuf(0, 1, 0x3F)},
                                  {gs_reg::trxdir, 0}})
                      .bytes);
        r.feed(1, image_packet(1, 0).bytes);
        r.feed(1, Packet::writes({{gs_reg::trxdir, 3}}).bytes);
        CHECK(r.events.uploads.size() == 1);
        CHECK(r.events.log.size() == 7 && r.events.log[1] == "read " + transfer_text(huge));
    }
    // no transfer begun: IMAGE data dropped
    {
        Rig r;
        r.feed(3, image_packet(4, 0).bytes);
        CHECK(r.events.log.empty());
    }
}

void test_truncated_and_malformed() {
    Rig r;
    // PACKED NLOOP 100 with two qwords: both written, then the path waits
    Packet p;
    p.tag(100, packed, 1, 0xE);
    p.ad(gs_reg::frame_1, 1);
    p.ad(gs_reg::frame_2, 2);
    std::vector<uint8_t> bytes = p.bytes;
    bytes.push_back(0xFF); // (a part qword: not read)
    bytes.push_back(0xFF);
    r.feed(1, bytes);
    CHECK(r.events.log.size() == 2);
    // the other paths go on
    r.feed(2, Packet::writes({{gs_reg::zbuf_1, 3}}).bytes);
    CHECK(r.events.log.size() == 3);
    // paths out of range, empty feeds
    r.feed(-1, Packet::writes({{gs_reg::zbuf_1, 4}}).bytes);
    r.feed(4, Packet::writes({{gs_reg::zbuf_1, 4}}).bytes);
    r.feed(99, {});
    r.feed(1, {});
    CHECK(r.events.log.size() == 3);
    // IMAGE NLOOP 0x7FFF with a qword
    r.feed(3, Packet::writes({{gs_reg::bitbltbuf, bitbltbuf(0, 1, 0)}, {gs_reg::trxreg, trxreg(4, 4)},
                              {gs_reg::trxdir, 0}})
                  .bytes);
    Packet big;
    big.tag(0x7FFF, image, 0, 0);
    big.qword(1, 2);
    r.feed(3, big.bytes);
    CHECK(r.events.uploads.empty());
}

// The same packets split into two feeds at every qword, and into single qwords: the same events
void test_splits() {
    Packet p = Packet::writes({{gs_reg::prim, 4}, {gs_reg::bitbltbuf, bitbltbuf(5, 1, 0)},
                               {gs_reg::trxreg, trxreg(4, 4)}, {gs_reg::trxdir, 0}, {gs_reg::tex0_1, 77}});
    Packet img = image_packet(4, 3);
    p.bytes.insert(p.bytes.end(), img.bytes.begin(), img.bytes.end());
    p.tag(3, packed, 3, 0x521, 4);
    for (uint32_t i = 0; i < 3; i++) {
        p.qword(float_bits(float(i)), float_bits(0.5f));
        p.qword(i, i + 1);
        packed_xyz(p, i * 16, i, i, i == 1);
    }
    p.tag(2, reglist, 3, 0x515);
    for (uint32_t i = 0; i < 3; i++) {
        p.qword(xyz(i, i, i), 0x1000 + i);
    }
    Rig whole;
    whole.feed(1, p.bytes);
    CHECK(whole.events.uploads.size() == 1 && whole.events.primitives.size() > 2);
    const size_t qwords = p.bytes.size() / 16;
    for (size_t k = 0; k <= qwords; k++) {
        Rig split;
        split.feed(1, std::vector<uint8_t>(p.bytes.begin(), p.bytes.begin() + ptrdiff_t(k * 16)));
        split.feed(1, std::vector<uint8_t>(p.bytes.begin() + ptrdiff_t(k * 16), p.bytes.end()));
        CHECK(split.events.log == whole.events.log);
    }
    Rig single;
    for (size_t k = 0; k < qwords; k++) {
        single.feed(1, std::vector<uint8_t>(p.bytes.begin() + ptrdiff_t(k * 16), p.bytes.begin() + ptrdiff_t(k * 16 + 16)));
    }
    CHECK(single.events.log == whole.events.log);
}

// ---- The reference walk: from the packets' structure, as the GS is described ------------------------------------

// What one qword of a packet does, known from how it was made (not read back from its bytes)
struct Op {
    enum Kind { tag, packed_register, reglist_registers, image_data } kind = tag;
    std::optional<uint32_t> pre_prim;     // tag
    uint32_t descriptor = 0;              // packed_register
    std::vector<uint32_t> reglist;        // reglist_registers: one or two descriptors
    uint64_t low = 0, high = 0;
};

struct Stream {
    std::vector<uint8_t> bytes;
    std::vector<Op> ops; // one a qword
};

class Reference {
public:
    std::vector<std::string> log;
    std::array<uint64_t, 256> reg{};
    uint32_t q = 0x3F800000u;

    void run(const Op &op) {
        switch (op.kind) {
        case Op::tag:
            q = 0x3F800000u;
            if (op.pre_prim) {
                write(gs_reg::prim, *op.pre_prim);
            }
            return;
        case Op::packed_register:
            packed(op.descriptor, op.low, op.high);
            return;
        case Op::reglist_registers:
            for (size_t i = 0; i < op.reglist.size(); i++) {
                if (op.reglist[i] < 0xE) {
                    write(op.reglist[i], i == 0 ? op.low : op.high);
                }
            }
            return;
        case Op::image_data: {
            uint8_t b[16];
            std::memcpy(b, &op.low, 8);
            std::memcpy(b + 8, &op.high, 8);
            data(std::span<const uint8_t>(b, 16));
            return;
        }
        }
    }

private:
    struct Upload {
        TransferRegisters t;
        size_t size;
        std::vector<uint8_t> bytes;
    };
    std::vector<GifVertex> queue;
    std::optional<Upload> upload;

    static uint64_t field(uint64_t v, unsigned at, unsigned n) { return (v >> at) & ((uint64_t(1) << n) - 1); }

    void changed(uint32_t a, uint64_t v) {
        if (reg[a] != v) {
            log.push_back("reg " + hex(a) + " " + hex(reg[a]) + " " + hex(v));
            reg[a] = v;
        }
    }

    void packed(uint32_t d, uint64_t lo, uint64_t hi) {
        const uint64_t xy = field(lo, 0, 16) | field(lo, 32, 16) << 16;
        const bool adc = field(hi, 47, 1);
        const uint64_t xyzf = xy | field(hi, 4, 24) << 32 | field(hi, 36, 8) << 56;
        const uint64_t xyz32 = xy | field(hi, 0, 32) << 32;
        if (d == 0) {
            write(0x00, field(lo, 0, 11));
        } else if (d == 1) {
            write(0x01, field(lo, 0, 8) | field(lo, 32, 8) << 8 | field(hi, 0, 8) << 16 | field(hi, 32, 8) << 24 |
                            uint64_t(q) << 32);
        } else if (d == 2) {
            q = uint32_t(hi);
            write(0x02, lo);
        } else if (d == 3) {
            write(0x03, field(lo, 0, 14) | field(lo, 32, 14) << 16);
        } else if (d == 4) {
            write(adc ? 0x0C : 0x04, xyzf);
        } else if (d == 5) {
            write(adc ? 0x0D : 0x05, xyz32);
        } else if (d == 0xA) {
            write(0x0A, field(hi, 36, 8) << 56);
        } else if (d == 0xC) {
            write(0x0C, xyzf);
        } else if (d == 0xD) {
            write(0x0D, xyz32);
        } else if (d == 0xE) {
            write(uint32_t(hi & 0xFF), lo);
        } else if (d != 0xF) {
            write(d, lo);
        }
    }

    void tex0(uint32_t c, uint64_t v) {
        reg[6 + c] = v;
        if (field(reg[0x14 + c], 9, 1)) {
            const uint64_t psm = field(v, 20, 6);
            const bool is16 = psm == 2 || psm == 0x0A || psm == 0x32 || psm == 0x3A;
            const uint32_t bw = psm == 0x14 ? 32 : (psm == 0x13 || is16) ? 16 : 8;
            const uint32_t bh = (psm == 0x13 || psm == 0x14) ? 16 : 8;
            const uint32_t w = 1u << field(v, 26, 4), h = 1u << field(v, 30, 4);
            uint32_t base = uint32_t(field(v, 0, 14)), blocks = (w / bw) * (h / bh), row = w / 64;
            uint64_t m = 0;
            for (unsigned l = 0; l < 3; l++) {
                base += blocks;
                blocks /= 4;
                row /= 2;
                m |= (uint64_t(base & 0x3FFF) | uint64_t(row & 0x3F) << 14) << (20 * l);
            }
            changed(0x34 + c, m);
        }
        log.push_back("tex " + hex(c) + " " + hex(v) + " " + hex(reg[0x1C]));
    }

    void write(uint32_t a, uint64_t v) {
        if (a == 0x04 || a == 0x05 || a == 0x0C || a == 0x0D) {
            kick(a, v);
        } else if (a == 0x01 || a == 0x02 || a == 0x03 || a == 0x0A) {
            reg[a] = v;
        } else if (a == 0x00) {
            queue.clear();
            changed(0, v & 0x7FF);
        } else if (a == 0x06 || a == 0x07) {
            tex0(a - 6, v);
        } else if (a == 0x16 || a == 0x17) {
            const uint64_t mask = uint64_t(0x3F) << 20 | ~uint64_t(0) << 37;
            tex0(a - 0x16, (reg[a - 0x10] & ~mask) | (v & mask));
        } else if (a == 0x3F || a == 0x0F) {
        } else if (a == 0x7B) {
            log.push_back("native " + hex(v));
        } else if (a == 0x7C) {
            log.push_back("again " + hex(v));
        } else if (a == 0x7D) {
            log.push_back("mark " + hex(v));
        } else if (a == 0x7E) {
            log.push_back("wide " + hex(v));
        } else if (a >= 0x60 && a <= 0x62) {
            log.push_back("sync " + hex(a) + " " + hex(v));
        } else if (a == 0x53) {
            changed(a, v);
            transfer(uint32_t(v & 3));
        } else if (a == 0x54) {
            uint8_t b[8];
            std::memcpy(b, &v, 8);
            data(std::span<const uint8_t>(b, 8));
        } else {
            changed(a, v);
        }
    }

    // How many vertices make each type's primitive, and what of the queue stays after one
    enum Keep { none, drop_first, drop_second }; // (strips keep their last ones, fans their first and last)
    struct Rule {
        uint32_t vertices;
        Keep keep;
        PrimitiveShape shape;
    };

    void kick(uint32_t a, uint64_t v) {
        static const Rule rules[8] = {{1, none, PrimitiveShape::point},     {2, none, PrimitiveShape::line},
                                      {2, drop_first, PrimitiveShape::line},      {3, none, PrimitiveShape::triangle},
                                      {3, drop_first, PrimitiveShape::triangle},
                                      {3, drop_second, PrimitiveShape::triangle},
                                      {2, none, PrimitiveShape::sprite},    {0, none, PrimitiveShape::point}};
        const bool f = a == 0x04 || a == 0x0C;
        GifVertex x;
        x.x = uint16_t(v);
        x.y = uint16_t(v >> 16);
        x.z = f ? uint32_t(field(v, 32, 24)) : uint32_t(v >> 32);
        x.fog = uint8_t(f ? v >> 56 : reg[0x0A] >> 56);
        x.rgba = uint32_t(reg[1]);
        x.q = std::bit_cast<float>(uint32_t(reg[1] >> 32));
        x.s = std::bit_cast<float>(uint32_t(reg[2]));
        x.t = std::bit_cast<float>(uint32_t(reg[2] >> 32));
        x.u = uint16_t(field(reg[3], 0, 14));
        x.v = uint16_t(field(reg[3], 16, 14));
        uint32_t prim = uint32_t(reg[0] & 0x7FF);
        if (!(reg[0x1A] & 1)) {
            prim = (prim & 7) | uint32_t(reg[0x1B] & 0x7F8);
        }
        const Rule rule = rules[prim & 7];
        if (rule.vertices == 0) {
            queue.clear();
            return;
        }
        queue.push_back(x);
        if (queue.size() < rule.vertices) {
            return;
        }
        if (a == 0x04 || a == 0x05) {
            GifPrimitive p;
            p.shape = rule.shape;
            p.prim = prim;
            p.vertex_count = rule.vertices;
            for (uint32_t i = 0; i < rule.vertices; i++) {
                p.vertices[i] = queue[i];
            }
            log.push_back(primitive_text(p, reg[6 + ((prim >> 9) & 1)]));
        }
        if (rule.keep == none) {
            queue.clear();
        } else if (rule.keep == drop_first) {
            queue.erase(queue.begin());
        } else {
            queue.erase(queue.begin() + 1);
        }
    }

    void transfer(uint32_t dir) {
        if (upload) {
            log.push_back(upload_text(upload->t, upload->bytes, UploadEnd::cut_short));
            upload.reset();
        }
        TransferRegisters t = {reg[0x50], reg[0x51], reg[0x52]};
        if (dir == 1) {
            log.push_back("read " + transfer_text(t));
        } else if (dir == 2) {
            log.push_back("copy " + transfer_text(t));
        } else if (dir == 0) {
            static const std::pair<uint32_t, uint32_t> psm_bits[] = {
                {0x00, 32}, {0x01, 24}, {0x02, 16}, {0x0A, 16}, {0x13, 8},  {0x14, 4},  {0x1B, 8},
                {0x24, 4},  {0x2C, 4},  {0x30, 32}, {0x31, 24}, {0x32, 16}, {0x3A, 16}};
            uint64_t bpp = 0;
            for (auto [psm, b] : psm_bits) {
                if (psm == field(t.bitbltbuf, 56, 6)) {
                    bpp = b;
                }
            }
            const uint64_t size = (field(t.trxreg, 0, 12) * field(t.trxreg, 32, 12) * bpp + 7) / 8;
            if (size != 0) {
                upload = Upload{t, size_t(size), {}};
            }
        }
    }

    void data(std::span<const uint8_t> b) {
        if (!upload) {
            return;
        }
        for (uint8_t byte : b) {
            if (upload->bytes.size() < upload->size) {
                upload->bytes.push_back(byte);
            }
        }
        if (upload->bytes.size() == upload->size) {
            log.push_back(upload_text(upload->t, upload->bytes, UploadEnd::complete));
            upload.reset();
        }
    }
};

// ---- Random packet streams --------------------------------------------------------------------------------------

class Maker {
public:
    explicit Maker(uint64_t seed) : rng(seed) {}

    // A path's stream: `packets` packets, the last cut short at a random qword
    Stream stream(int packets) {
        Stream s;
        for (int i = 0; i < packets; i++) {
            packet(s);
        }
        const size_t cut = below(4);
        for (size_t i = 0; i < cut && !s.ops.empty(); i++) {
            s.ops.pop_back();
            s.bytes.resize(s.bytes.size() - 16);
        }
        return s;
    }

    uint64_t below(uint64_t n) { return rng() % n; }
    uint64_t any() { return rng(); }

private:
    std::mt19937_64 rng;

    void put(Stream &s, Op op) {
        uint8_t q[16];
        std::memcpy(q, &op.low, 8);
        std::memcpy(q + 8, &op.high, 8);
        s.bytes.insert(s.bytes.end(), q, q + 16);
        s.ops.push_back(std::move(op));
    }

    // A register's value as the game would write it, or now and then any bits
    uint64_t value_for(uint32_t address) {
        if (below(8) == 0) {
            return any();
        }
        switch (address) {
        case gs_reg::prim:
            return below(0x800);
        case gs_reg::trxdir:
            return below(2) ? 0 : below(4); // (mostly host to local)
        case gs_reg::bitbltbuf: {
            static const uint32_t psms[] = {0x00, 0x01, 0x02, 0x0A, 0x13, 0x14, 0x1B, 0x24, 0x2C, 0x30, 0x3F};
            return (any() & 0xFFFFFFFFull) | uint64_t(below(0x4000)) << 32 | uint64_t(below(64)) << 48 |
                   uint64_t(psms[below(11)]) << 56;
        }
        case gs_reg::trxreg:
            return below(16) == 0 ? trxreg(4095, 4095) : trxreg(uint32_t(below(5)), uint32_t(below(3)));
        case gs_reg::tex1_1:
        case gs_reg::tex1_2:
            return below(2) << 9;
        case gs_reg::prmodecont:
            return below(2);
        case gs_reg::tex0_1:
        case gs_reg::tex0_2:
            return (any() & ~(uint64_t(0xFF) << 26)) | below(11) << 26 | below(11) << 30;
        }
        return any();
    }

    uint32_t ad_address() {
        static const uint32_t pool[] = {gs_reg::prim,       gs_reg::rgbaq,     gs_reg::st,        gs_reg::uv,
                                        gs_reg::xyzf2,      gs_reg::xyz2,      gs_reg::xyzf3,     gs_reg::xyz3,
                                        gs_reg::xyz2,       gs_reg::xyzf2,     gs_reg::tex0_1,    gs_reg::tex0_2,
                                        gs_reg::tex1_1,     gs_reg::tex2_1,    gs_reg::tex2_2,    gs_reg::fog,
                                        gs_reg::prmodecont, gs_reg::prmode,    gs_reg::texclut,   gs_reg::frame_1,
                                        gs_reg::bitbltbuf,  gs_reg::trxpos,    gs_reg::trxreg,    gs_reg::trxdir,
                                        gs_reg::trxdir,     gs_reg::hwreg,     gs_reg::hwreg,     gs_reg::native_draw,
                                        gs_reg::upload_again, gs_reg::frame_mark, gs_reg::widescreen, gs_reg::signal,
                                        gs_reg::finish,     gs_reg::label,     gs_reg::texflush,  0x0F};
        return below(16) == 0 ? uint32_t(below(256)) : pool[below(sizeof(pool) / sizeof(pool[0]))];
    }

    // A PACKED qword for the descriptor
    Op packed_qword(uint32_t d) {
        Op op;
        op.kind = Op::packed_register;
        op.descriptor = d;
        op.low = any();
        op.high = any();
        if (d == 0) {
            op.low = value_for(gs_reg::prim);
        } else if (d == 0xE) {
            const uint32_t a = ad_address();
            op.low = value_for(a);
            op.high = (any() & ~uint64_t(0xFF)) | a;
        } else if (d == 6 || d == 7) {
            op.low = value_for(gs_reg::tex0_1);
        } else if (d == 4 || d == 5) {
            op.high = (op.high & ~(uint64_t(1) << 47)) | uint64_t(below(4) == 0) << 47;
        }
        return op;
    }

    void packet(Stream &s) {
        const int tags = 1 + int(below(3));
        for (int t = 0; t < tags; t++) {
            const uint32_t flg = uint32_t(below(4));
            const uint32_t nloop = uint32_t(below(flg >= image ? 9 : 5));
            const uint32_t nreg_field = uint32_t(below(17)) & 15; // (0 is 16)
            const uint32_t nreg = nreg_field == 0 ? 16 : nreg_field;
            static const uint32_t descriptors[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0xA, 0xB, 0xC, 0xD, 0xE, 0xE, 0xE, 0xF};
            uint64_t regs = 0;
            for (uint32_t r = 0; r < 16; r++) {
                regs |= uint64_t(descriptors[below(sizeof(descriptors) / sizeof(descriptors[0]))]) << (4 * r);
            }
            Op tag;
            tag.kind = Op::tag;
            tag.high = regs;
            tag.low = uint64_t(nloop) | uint64_t(t + 1 == tags) << 15 | uint64_t(flg) << 58 |
                      uint64_t(nreg_field) << 60;
            if (flg == packed && below(2)) {
                const uint32_t prim = uint32_t(below(0x800));
                tag.low |= uint64_t(1) << 46 | uint64_t(prim) << 47;
                tag.pre_prim = prim;
            } else if (flg != packed && below(2)) {
                tag.low |= uint64_t(1) << 46 | below(0x800) << 47; // (PRE outside PACKED: not written)
            }
            put(s, tag);
            if (flg == packed) {
                for (uint32_t i = 0; i < nloop * nreg; i++) {
                    put(s, packed_qword(uint32_t((regs >> (4 * (i % nreg))) & 15)));
                }
            } else if (flg == reglist) {
                const uint32_t dwords = nloop * nreg;
                for (uint32_t i = 0; i < dwords; i += 2) {
                    Op op;
                    op.kind = Op::reglist_registers;
                    for (uint32_t h = 0; h < 2 && i + h < dwords; h++) {
                        const uint32_t d = uint32_t((regs >> (4 * ((i + h) % nreg))) & 15);
                        op.reglist.push_back(d);
                        (h == 0 ? op.low : op.high) = value_for(d);
                    }
                    if (op.reglist.size() == 1) {
                        op.high = any();
                    }
                    put(s, op);
                }
            } else {
                for (uint32_t i = 0; i < nloop; i++) {
                    Op op;
                    op.kind = Op::image_data;
                    op.low = any();
                    op.high = any();
                    put(s, op);
                }
            }
        }
    }
};

// Three paths' streams, fed in random pieces taken from the paths in random order; the reference runs each piece's
// qwords as it is fed
// (how many of each event the seeds made, so the comparison is known to have covered them)
struct Tally {
    size_t events = 0, primitives = 0, uploads = 0, cut = 0, textures = 0, copies = 0;
};

bool random_streams(uint64_t seed, Tally &tally) {
    Maker make(seed);
    Stream streams[3];
    for (Stream &s : streams) {
        s = make.stream(30 + int(make.below(30)));
    }
    Rig rig;
    Reference ref;
    size_t at[3] = {0, 0, 0};
    for (;;) {
        int open[3], n_open = 0;
        for (int p = 0; p < 3; p++) {
            if (at[p] < streams[p].ops.size()) {
                open[n_open++] = p;
            }
        }
        if (n_open == 0) {
            break;
        }
        const int p = open[make.below(uint64_t(n_open))];
        const size_t left = streams[p].ops.size() - at[p];
        const size_t n = make.below(6) == 0 ? left : 1 + size_t(make.below(std::min<size_t>(left, 9)));
        std::vector<uint8_t> piece(streams[p].bytes.begin() + ptrdiff_t(at[p] * 16),
                                   streams[p].bytes.begin() + ptrdiff_t((at[p] + n) * 16));
        for (uint64_t junk = make.below(16) * (make.below(4) == 0); junk > 0; junk--) {
            piece.push_back(uint8_t(make.any())); // (a part qword after the piece: not read)
        }
        rig.feed(p + 1, piece);
        for (size_t i = 0; i < n; i++) {
            ref.run(streams[p].ops[at[p] + i]);
        }
        at[p] += n;
    }
    for (const std::string &e : rig.events.log) {
        tally.events++;
        tally.primitives += e.starts_with("prim");
        tally.uploads += e.starts_with("upload") && e.find(" complete ") != std::string::npos;
        tally.cut += e.starts_with("upload") && e.find(" cut ") != std::string::npos;
        tally.textures += e.starts_with("tex");
        tally.copies += e.starts_with("copy") || e.starts_with("read");
    }
    const bool same = rig.events.log == ref.log && rig.gif.registers().reg == ref.reg &&
                      rig.gif.registers().internal_q == ref.q;
    if (!same) {
        size_t i = 0;
        while (i < rig.events.log.size() && i < ref.log.size() && rig.events.log[i] == ref.log[i]) {
            i++;
        }
        std::printf("seed %" PRIu64 ": %zu events, reference %zu, first difference at %zu\n", seed,
                    rig.events.log.size(), ref.log.size(), i);
        if (i < rig.events.log.size()) {
            std::printf("  front end: %s\n", rig.events.log[i].c_str());
        }
        if (i < ref.log.size()) {
            std::printf("  reference: %s\n", ref.log[i].c_str());
        }
    }
    return same;
}

void test_random_streams() {
    Tally tally;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        CHECK(random_streams(seed, tally));
    }
    std::printf("random streams: 200 seeds, %zu events (%zu primitives, %zu uploads whole, %zu cut short, %zu TEX0, "
                "%zu copies and read-backs)\n",
                tally.events, tally.primitives, tally.uploads, tally.cut, tally.textures, tally.copies);
    CHECK(tally.primitives > 1000 && tally.uploads > 200 && tally.cut > 100 && tally.textures > 1000);
}

// Random bytes on any path number, in pieces of any length: nothing may crash or read past the data, and what
// comes out stays within what the GS can give
void test_random_bytes() {
    std::mt19937_64 rng(77);
    class Bounds : public Recorder {
    public:
        bool ok = true;
        void primitive(const GifPrimitive &p) override { ok = ok && p.vertex_count >= 1 && p.vertex_count <= 3; }
        void image_upload(const TransferRegisters &t, std::span<const uint8_t> bytes, UploadEnd) override {
            const uint64_t most = uint64_t(4095) * 4095 * 4;
            ok = ok && bytes.size() <= most && (t.trxreg & 0xFFF) != 0;
        }
    } events;
    GifFrontEnd gif(events);
    events.front_end = &gif;
    for (int round = 0; round < 20000; round++) {
        std::vector<uint8_t> bytes(rng() % 200);
        for (uint8_t &b : bytes) {
            b = uint8_t(rng());
        }
        gif.feed(int(rng() % 7) - 1, bytes);
    }
    CHECK(events.ok);
}

} // namespace

int main() {
    test_register_writes();
    test_primitive_types();
    test_xyz2_and_xyz3();
    test_prim_resets_queue();
    test_prmode();
    test_q_latching();
    test_uv_and_fog_packed();
    test_kick_numbers();
    test_tex2_merge();
    test_mip_addresses();
    test_reglist();
    test_uploads();
    test_truncated_and_malformed();
    test_splits();
    test_random_streams();
    test_random_bytes();
    if (failures) {
        std::printf("pcr_gif: %d failed\n", failures);
        return 1;
    }
    std::printf("pcr_gif: all passed\n");
    return 0;
}
