// The renderer core of native draw (port/include/port/render.h): meshes parsed from PS2 data blocks and
// cached, and the per-frame side buffer of draws.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "render_internal.hpp"

#include <unordered_set>

extern "C" {
#include "port/gs_capture_format.h"
}

RdStats rd_stats;

namespace {

// ---- Parsing a block ----------------------------------------------------------------------------
// A block is a stream of VIF codes, as VIF1 reads it (port/platform/vif1.c is the model): UNPACKs into
// VU1 memory relative to TOPS (FLG), and MSCAL/MSCNT, each a VU1 run of the batch at TOP. VU1 memory
// is modelled as far as the batches need it: two buffers for TOPS to alternate between, and which of
// their qwords this block has written. A batch whose tag, vertices or A+D packet the block did not
// write itself (left by the packets before it on the PS2) is not something the block alone describes:
// the block is refused.
const uint32_t VU_QWORDS = 1024;
const uint32_t BUFFER_A = 0x080, BUFFER_B = 0x240; // (the game's own TOPs: Packet_LayOutBuffers' BASE/OFFSET)
const uint32_t N_MAX = 39;                         // vu1_hle.c: more and the vertices reach TOP+0xA0

const char *why_refused = "";

struct VuModel {
    uint32_t mem[VU_QWORDS][4];
    uint8_t written[VU_QWORDS]; // by the block, since its last run
    uint8_t ever[VU_QWORDS];    // by the block, in any of its runs (program 0x700's vertex n: RdBatch::past)
};

uint32_t unpack_bits(uint32_t cmd) {
    uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
    return vl == 3 ? 16 : (vn + 1) * (32u >> vl);
}

uint32_t element(const uint8_t *bytes, uint32_t index, uint32_t vl, bool usn) {
    switch (vl) {
    case 0: {
        uint32_t v;
        memcpy(&v, bytes + index * 4, 4);
        return v;
    }
    case 1: {
        uint16_t v;
        memcpy(&v, bytes + index * 2, 2);
        return usn ? v : uint32_t(int32_t(int16_t(v)));
    }
    default: {
        uint8_t v = bytes[index];
        return usn ? v : uint32_t(int32_t(int8_t(v)));
    }
    }
}

// The batch at top (a VU1 run of program 0x000's shape: vu1_hle.c), into mesh. silhouette: a run of program
// 0x5A0 instead (RD_VF_SILHOUETTE; port/platform/vu1_hle_5a0.c is its specification), on the same blocks: it
// is 0x000's transform, projection, clipping, ADC and ST by q, but it kicks no A+D packet (the batch has
// none), its colour is 0 (no lights: RGBAQ 0), its fog 0 (clamped to 0..0), and ST's z is 1.0 for every
// vertex but the last two, which keep the data's. So the batch is stored as program 0x000 would draw it to
// give the same: each vertex's colour qword 0 and its ST's z 4096 (ITOF12: exactly 1.0) but the last two's;
// the draw's record has no lights and no fog, and the program 0x000 shader draws it.
bool take_batch(const VuModel &vu, uint32_t top, uint32_t start, RdMeshData &mesh, bool silhouette = false) {
    if (silhouette && start != RD_PROGRAM_CONTINUE && start != 0x5A0) {
        why_refused = "an MSCAL of another program in a 0x5A0 block";
        return false;
    }
    if (!vu.written[top]) {
        why_refused = "a batch's GIF tag is not the block's";
        return false;
    }
    RdBatch b;
    memcpy(b.tag, vu.mem[top], 16);
    uint32_t n = uint32_t(b.tag[0] & 0x7FFF);
    uint32_t nreg = uint32_t(b.tag[0] >> 60), flg = uint32_t((b.tag[0] >> 58) & 3);
    if (n < 3 || n > N_MAX) {
        why_refused = "a batch of fewer than 3 or more than 39 vertices";
        return false;
    }
    if (flg != 0 || nreg != 3 || (b.tag[1] & 0xFFF) != 0x412) {
        why_refused = "a batch whose GIF tag is not ST, RGBAQ, XYZF2";
        return false;
    }
    if (uint16_t(b.tag[1] >> 32) != 0) {
        why_refused = "a batch tag with w set (vu1_hle.c's ADC for every vertex)";
        return false;
    }
    for (uint32_t q = top + 1; q < top + 1 + 4 * n; q++) {
        if (!vu.written[q % VU_QWORDS]) {
            why_refused = "a batch's vertices are not all the block's";
            return false;
        }
    }
    // the A+D packet at TOP+0xA0: a GIF tag (PACKED, A+D) and its writes (program 0x5A0 kicks none)
    uint32_t ad = top + 0xA0;
    if (silhouette) {
        // (none)
    } else if (!vu.written[ad % VU_QWORDS]) {
        why_refused = "a batch's A+D packet is not the block's";
        return false;
    }
    uint64_t adtag[2] = {0, 0};
    if (!silhouette) {
        memcpy(adtag, vu.mem[ad % VU_QWORDS], 16);
    }
    uint32_t ad_n = uint32_t(adtag[0] & 0x7FFF), ad_nreg = uint32_t(adtag[0] >> 60);
    if (!silhouette && (((adtag[0] >> 58) & 3) != 0 || ad_nreg != 1 || (adtag[1] & 0xF) != 0xE || ad_n > 16)) {
        why_refused = "a batch's A+D packet is not A+D";
        return false;
    }
    for (uint32_t i = 0; i < ad_n; i++) {
        uint32_t q = (ad + 1 + i) % VU_QWORDS;
        if (!vu.written[q]) {
            why_refused = "a batch's A+D writes are not all the block's";
            return false;
        }
        uint64_t w[2];
        memcpy(w, vu.mem[q], 16);
        b.ad.push_back(w[1] & 0xFF);
        b.ad.push_back(w[0]);
    }
    b.start = start;
    b.first = uint32_t(mesh.vertices.size() / 16);
    b.count = n;
    {
        uint32_t past = (top + 1 + 4 * n) % VU_QWORDS;
        b.past_known = vu.ever[past] != 0;
        memcpy(b.past, vu.mem[past], 16);
    }
    b.alpha_lo = 255;
    b.alpha_hi = 0;
    for (uint32_t k = 0; k < n; k++) {
        for (uint32_t q = 0; q < 4; q++) {
            const uint32_t *src = vu.mem[(top + 1 + 4 * k + q) % VU_QWORDS];
            for (int f = 0; f < 4; f++) {
                mesh.vertices.push_back(int32_t(src[f]));
            }
        }
        if (silhouette) {
            int32_t *v = &mesh.vertices[mesh.vertices.size() - 16];
            if (k + 2 < n) {
                v[10] = 4096; // (ST's z 1.0)
            }
            v[12] = v[13] = v[14] = v[15] = 0; // (the colour: 0)
        }
        // (the colour's alpha as VU1 leaves it: min(1 * a, 255), FTOI0)
        int32_t a = silhouette ? 0 : int32_t(vu.mem[(top + 4 + 4 * k) % VU_QWORDS][3]);
        uint32_t ac = a < 0 ? 0u : a > 255 ? 255u : uint32_t(a);
        b.alpha_lo = std::min(b.alpha_lo, ac);
        b.alpha_hi = std::max(b.alpha_hi, ac);
    }
    // The triangles, as the GS queues the kicks (the GIF front end, port/gs/pc/pcr_gif.cpp): a kick whose XYZF2 has ADC set
    // by the data (the normal's w, ORed into it by the program) draws nothing. Clipping's ADC is the
    // frame's.
    uint32_t prim = uint32_t((b.tag[0] >> 47) & 0x7FF);
    if (!((b.tag[0] >> 46) & 1)) {
        why_refused = "a batch tag without PRE";
        return false;
    }
    uint32_t type = prim & 7;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t nw = uint16_t(vu.mem[(top + 2 + 4 * k) % VU_QWORDS][3]);
        bool adc = (nw & 0x8000) != 0;
        int a = -1, bb = -1;
        if (type == 3 && k % 3 == 2) {
            a = int(k) - 2;
            bb = int(k) - 1;
        } else if (type == 4 && k >= 2) {
            a = int(k) - 2;
            bb = int(k) - 1;
        } else if (type == 5 && k >= 2) {
            a = 0;
            bb = int(k) - 1;
        } else if (type != 3 && type != 4 && type != 5) {
            why_refused = "a batch of points, lines or sprites";
            return false;
        }
        if (a >= 0 && !adc) {
            b.triangles.push_back(uint8_t(a));
            b.triangles.push_back(uint8_t(bb));
            b.triangles.push_back(uint8_t(k));
        }
    }
    mesh.batches.push_back(std::move(b));
    return true;
}

// ---- Program 0x280's batches (RD_VF_FLOAT_SKIN: the skinned models) ------------------------------
// One VU1 run of program 0x280 (port/platform/vu1_hle_280.c is its specification) at top: TOP+0 the
// strip's GIF tag, TOP+1 the second pass's, TOP+2+4k vertex k (position, normal, ST, colour: floats;
// the normal's w ORed into XYZF2's w, but the last vertex's; ST's w the matrix set), TOP+0xA0 the A+D
// packet kicked before the strip, and the second (sphere-mapped) pass when TOP+0xA4.z's low 16 bits
// are not 0: then the A+D packet at TOP+0xA3 and a strip of the same XYZF2s under the tag at TOP+1.
const uint32_t N_MAX_STRIP = 64; // (the trail: WeaponTrail_Push keeps 64 points at most; a triangle's numbers are bytes)
const uint32_t N_MAX_280 = 36; // (vu1_hle_280.c: the HLE leaves more to the interpreter; none in the game)

// a GIF tag of a strip of n vertices of ST, RGBAQ, XYZF2 with PRE; why not, or nullptr
const char *strip_tag_280(const uint64_t tag[2], uint32_t n) {
    uint32_t nreg = uint32_t(tag[0] >> 60), flg = uint32_t((tag[0] >> 58) & 3);
    if (uint32_t(tag[0] & 0x7FFF) != n) {
        return "a 0x280 batch whose second pass's tag has another NLOOP";
    }
    if (flg != 0 || nreg != 3 || (tag[1] & 0xFFF) != 0x412) {
        return "a 0x280 batch whose GIF tag is not ST, RGBAQ, XYZF2";
    }
    if (!((tag[0] >> 46) & 1)) {
        return "a 0x280 batch tag without PRE";
    }
    uint32_t type = uint32_t(tag[0] >> 47) & 7;
    if (type != 3 && type != 4 && type != 5) {
        return "a 0x280 batch of points, lines or sprites";
    }
    return nullptr;
}

// the A+D packet at address (a GIF tag, PACKED A+D, and its writes) into ad; why not, or nullptr
const char *ad_packet_280(const VuModel &vu, uint32_t address, std::vector<uint64_t> &ad) {
    if (!vu.written[address % VU_QWORDS]) {
        return "a 0x280 batch's A+D packet is not the block's";
    }
    uint64_t tag[2];
    memcpy(tag, vu.mem[address % VU_QWORDS], 16);
    uint32_t n = uint32_t(tag[0] & 0x7FFF);
    if (((tag[0] >> 58) & 3) != 0 || (tag[0] >> 60) != 1 || (tag[1] & 0xF) != 0xE || n > 16) {
        return "a 0x280 batch's A+D packet is not A+D";
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t q = (address + 1 + i) % VU_QWORDS;
        if (!vu.written[q]) {
            return "a 0x280 batch's A+D writes are not all the block's";
        }
        uint64_t w[2];
        memcpy(w, vu.mem[q], 16);
        ad.push_back(w[1] & 0xFF);
        ad.push_back(w[0]);
    }
    return nullptr;
}

// The batch at top (a VU1 run of program 0x280), into mesh: its vertices eight qwords each, the four VU1
// memory has, then (k, its set for the point lights, what its XYZF2's w is ORed with, n) and three
// qwords of 0. (The point lights take vertex 0's set from the tag's w: vu1_hle_280.c, light_set.)
bool take_batch_280(const VuModel &vu, uint32_t top, uint32_t start, RdMeshData &mesh) {
    if (start != RD_PROGRAM_CONTINUE && start != 0x280) {
        why_refused = "an MSCAL of another program in a 0x280 block";
        return false;
    }
    if (!vu.written[top] || !vu.written[(top + 1) % VU_QWORDS]) {
        why_refused = "a 0x280 batch's GIF tags are not the block's";
        return false;
    }
    RdBatch b;
    memcpy(b.tag, vu.mem[top], 16);
    uint32_t n = uint32_t(b.tag[0] & 0x7FFF);
    if (n < 3 || n > N_MAX_280) {
        why_refused = "a 0x280 batch of fewer than 3 or more than 36 vertices";
        return false;
    }
    if (const char *why = strip_tag_280(b.tag, n)) {
        why_refused = why;
        return false;
    }
    for (uint32_t q = top + 2; q < top + 2 + 4 * n; q++) {
        if (!vu.written[q % VU_QWORDS]) {
            why_refused = "a 0x280 batch's vertices are not all the block's";
            return false;
        }
    }
    if (const char *why = ad_packet_280(vu, top + 0xA0, b.ad)) {
        why_refused = why;
        return false;
    }
    // the second pass: its gate, then its A+D packet and tag (the same PRIM type: the same triangles)
    if (!vu.written[(top + 0xA4) % VU_QWORDS]) {
        why_refused = "a 0x280 batch's sphere-pass gate (TOP+0xA4) is not the block's";
        return false;
    }
    b.sphere = uint16_t(vu.mem[(top + 0xA4) % VU_QWORDS][2]) != 0;
    if (b.sphere) {
        memcpy(b.tag2, vu.mem[(top + 1) % VU_QWORDS], 16);
        if (const char *why = strip_tag_280(b.tag2, n)) {
            why_refused = why;
            return false;
        }
        if (((b.tag2[0] ^ b.tag[0]) >> 47 & 7) != 0) {
            why_refused = "a 0x280 batch whose second pass is another kind of primitive";
            return false;
        }
        if (const char *why = ad_packet_280(vu, top + 0xA3, b.ad2)) {
            why_refused = why;
            return false;
        }
    }
    b.start = start;
    b.first = uint32_t(mesh.vertices.size() / 32);
    b.count = n;
    b.alpha_lo = 0; // (the alpha is the draw's colour scale and bias's: not known here)
    b.alpha_hi = 255;
    uint16_t tag_w = uint16_t(b.tag[1] >> 32);
    for (uint32_t k = 0; k < n; k++) {
        for (uint32_t q = 0; q < 4; q++) {
            const uint32_t *src = vu.mem[(top + 2 + 4 * k + q) % VU_QWORDS];
            for (int f = 0; f < 4; f++) {
                mesh.vertices.push_back(int32_t(src[f]));
            }
        }
        uint16_t set = uint16_t(vu.mem[(top + 4 + 4 * k) % VU_QWORDS][3]);
        uint16_t nw = uint16_t(vu.mem[(top + 3 + 4 * k) % VU_QWORDS][3]);
        mesh.vertices.push_back(int32_t(k));
        mesh.vertices.push_back(k == 0 ? tag_w != 0 : set != 0);
        mesh.vertices.push_back(k == n - 1 ? 0 : nw);
        mesh.vertices.push_back(int32_t(n));
        mesh.vertices.insert(mesh.vertices.end(), 12, 0);
    }
    // the triangles, as take_batch's (the last vertex's normal w is not ORed in: its ADC never the data's)
    uint32_t type = uint32_t(b.tag[0] >> 47) & 7;
    for (uint32_t k = 2; k < n; k++) {
        bool adc = k < n - 1 && (uint16_t(vu.mem[(top + 3 + 4 * k) % VU_QWORDS][3]) & 0x8000) != 0;
        if (adc || (type == 3 && k % 3 != 2)) {
            continue;
        }
        b.triangles.push_back(uint8_t(type == 5 ? 0 : k - 2));
        b.triangles.push_back(uint8_t(k - 1));
        b.triangles.push_back(uint8_t(k));
    }
    mesh.batches.push_back(std::move(b));
    return true;
}

// ---- A PATH2 strip (RD_VF_GS_STRIP: the weapon's trail, game/native_draw/nd_arm.c) ----------------
// The block is the GIF packet as the game built it: one PACKED tag with PRE, a triangle strip (PRIM's type
// 4), its registers RGBAQ and XYZ2, then NLOOP vertices of the two qwords. One batch: its tag, the
// vertices as the packet has them (eight words: RGBAQ's qword, XYZ2's; the ADC bit is XYZ2's bit 111,
// word 7 bit 15), the triangles the GS makes of them (a kick with ADC makes none), and the alpha range.
// The renderer writes them as the GIF would.
bool parse_gs_strip(const uint8_t *bytes, uint32_t qwords, RdMeshData &mesh) {
    if (qwords < 1) {
        why_refused = "a strip without its GIF tag";
        return false;
    }
    uint64_t tag[2];
    memcpy(tag, bytes, 16);
    uint32_t nloop = uint32_t(tag[0] & 0x7FFF), flg = uint32_t(tag[0] >> 58) & 3, nreg = uint32_t(tag[0] >> 60);
    if (flg != 0 || nreg != 2 || (tag[1] & 0xFF) != 0x51 || !((tag[0] >> 46) & 1) || ((tag[0] >> 47) & 7) != 4 ||
        !((tag[0] >> 15) & 1)) {
        why_refused = "not a PACKED strip of RGBAQ and XYZ2 with PRE and EOP";
        return false;
    }
    if (nloop > N_MAX_STRIP || 1 + 2 * nloop != qwords) {
        why_refused = "a strip whose NLOOP is not the block's size";
        return false;
    }
    RdBatch b;
    b.tag[0] = tag[0];
    b.tag[1] = tag[1];
    b.start = RD_PROGRAM_CONTINUE;
    b.first = uint32_t(mesh.vertices.size() / 8);
    b.count = nloop;
    b.alpha_lo = 255;
    b.alpha_hi = 0;
    const int32_t *v = reinterpret_cast<const int32_t *>(bytes + 16);
    mesh.vertices.insert(mesh.vertices.end(), v, v + size_t(nloop) * 8);
    for (uint32_t k = 0; k < nloop; k++) {
        uint32_t a = uint32_t(v[k * 8 + 3]) & 0xFF; // (PACKED RGBAQ: the GS takes the low 8 bits)
        b.alpha_lo = std::min(b.alpha_lo, a);
        b.alpha_hi = std::max(b.alpha_hi, a);
        if (k >= 2 && !(uint32_t(v[k * 8 + 7]) & 0x8000)) {
            b.triangles.push_back(uint8_t(k - 2));
            b.triangles.push_back(uint8_t(k - 1));
            b.triangles.push_back(uint8_t(k));
        }
    }
    mesh.batches.push_back(std::move(b));
    return true;
}

bool parse_block(const uint8_t *bytes, uint32_t qwords, RdMeshData &mesh, RdVertexFormat fmt = RD_VF_PACKED16) {
    if (fmt == RD_VF_GS_STRIP) {
        return parse_gs_strip(bytes, qwords, mesh);
    }
    static VuModel vu; // (the game's thread only)
    memset(vu.written, 0, sizeof(vu.written));
    memset(vu.ever, 0, sizeof(vu.ever));
    uint32_t cl = 1, wl = 1, mode = 0, tops = BUFFER_A;
    bool dbf = false;
    uint32_t row[4] = {};
    size_t words = size_t(qwords) * 4, i = 0;
    const uint32_t *w = reinterpret_cast<const uint32_t *>(bytes);
    while (i < words) {
        uint32_t code = w[i++];
        uint32_t cmd = (code >> 24) & 0x7F, num = (code >> 16) & 0xFF, imm = code & 0xFFFF;
        if (cmd >= 0x60) {
            uint32_t n = num ? num : 256;
            uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
            bool masked = (cmd & 0x10) != 0, usn = (imm >> 14) & 1;
            if (masked) {
                why_refused = "a masked UNPACK";
                return false;
            }
            if (wl == 0 || cl < wl) {
                why_refused = "an UNPACK with filling (CL < WL)";
                return false;
            }
            uint32_t need = (n * unpack_bits(cmd) + 31) / 32;
            if (i + need > words) {
                why_refused = "an UNPACK past the block's end";
                return false;
            }
            if (!((imm >> 15) & 1)) {
                why_refused = "an UNPACK to a fixed address (not relative to TOPS)";
                return false;
            }
            uint32_t address = (imm & 0x3FF) + tops;
            const uint8_t *data = reinterpret_cast<const uint8_t *>(w + i);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t q = (address + (k / wl) * cl + k % wl) % VU_QWORDS;
                uint32_t v[4];
                if (vl == 3) {
                    uint16_t h;
                    memcpy(&h, data + k * 2, 2);
                    v[0] = uint32_t(h & 0x1F) << 3;
                    v[1] = uint32_t((h >> 5) & 0x1F) << 3;
                    v[2] = uint32_t((h >> 10) & 0x1F) << 3;
                    v[3] = uint32_t((h >> 15) & 1) << 7;
                } else if (vn == 0) {
                    v[0] = v[1] = v[2] = v[3] = element(data, k, vl, usn);
                } else if (vn == 1) {
                    v[0] = v[2] = element(data, k * 2, vl, usn);
                    v[1] = v[3] = element(data, k * 2 + 1, vl, usn);
                } else if (vn == 2) {
                    v[0] = element(data, k * 3, vl, usn);
                    v[1] = element(data, k * 3 + 1, vl, usn);
                    v[2] = element(data, k * 3 + 2, vl, usn);
                    v[3] = (k + 1) * 3 < need * 32 / (32u >> vl) ? element(data, k * 3 + 3, vl, usn) : 0;
                } else {
                    for (uint32_t f = 0; f < 4; f++) {
                        v[f] = element(data, k * 4 + f, vl, usn);
                    }
                }
                for (int f = 0; f < 4; f++) {
                    if (mode == 1) {
                        v[f] += row[f];
                    } else if (mode == 2) {
                        row[f] += v[f];
                        v[f] = row[f];
                    }
                    vu.mem[q][f] = v[f];
                }
                vu.written[q] = 1;
                vu.ever[q] = 1;
            }
            i += need;
            continue;
        }
        switch (cmd) {
        case 0x00: // NOP
        case 0x06: // MSKPATH3
        case 0x07: // MARK
        case 0x10: // FLUSHE
        case 0x11: // FLUSH
        case 0x13: // FLUSHA
            break;
        case 0x01: // STCYCL
            cl = imm & 0xFF;
            wl = (imm >> 8) & 0xFF;
            break;
        case 0x05: // STMOD
            mode = imm & 3;
            break;
        case 0x02: // OFFSET, BASE, ITOP: the block's own double buffering is not modelled
        case 0x03:
        case 0x04:
            why_refused = "OFFSET, BASE or ITOP in a block";
            return false;
        case 0x14: // MSCAL
        case 0x15: // MSCALF
        case 0x17: { // MSCNT
            uint32_t start = cmd == 0x17 ? RD_PROGRAM_CONTINUE : imm;
            if (!(fmt == RD_VF_FLOAT_SKIN ? take_batch_280(vu, tops, start, mesh)
                                          : take_batch(vu, tops, start, mesh, fmt == RD_VF_SILHOUETTE))) {
                return false;
            }
            // (VIF1 flips TOPS at each run; what the block writes next goes to the other buffer)
            dbf = !dbf;
            tops = dbf ? BUFFER_B : BUFFER_A;
            memset(vu.written, 0, sizeof(vu.written));
            break;
        }
        case 0x20: // STMASK
            i += 1;
            break;
        case 0x30: // STROW
            if (i + 4 > words) {
                return false;
            }
            memcpy(row, w + i, 16);
            i += 4;
            break;
        case 0x31: // STCOL
            i += 4;
            break;
        default:
            why_refused = "a VIF code a block should not have (MPG, DIRECT ...)";
            return false;
        }
    }
    if (mesh.batches.empty()) {
        why_refused = "no MSCAL or MSCNT";
        return false;
    }
    return true;
}

uint64_t hash_bytes(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001B3ull;
    }
    return h;
}

// ---- The mesh cache (the game's thread) ---------------------------------------------------------
struct CacheEntry {
    uint32_t gen;      // the caller's, or the bytes' hash folded (RD_GEN_HASH)
    uint32_t touched;  // rd_mesh_touch since
    uint32_t frame;    // last asked for
    uint32_t format;   // the RdVertexFormat it was asked for (a block asked for as another is parsed again)
    std::shared_ptr<const RdMeshData> mesh; // (null: refused)
};
std::unordered_map<uint32_t, CacheEntry> cache;
std::unordered_map<RdMesh, std::shared_ptr<const RdMeshData>> by_id; // (the meshes cache holds)
RdMesh next_id = 1;

// ---- KF4_ND_DUMP=FILE (debug only; NATIVE_DRAW.md's open questions, P0-3) ------------------------
// A text file of what the core is handed, for tools/nd_measure.py: per frame "F serial parsed reparsed
// hashed_bytes hash_ns parse_ns"; per new mesh "M id key batches", then per batch "B start prim alpha_lo alpha_hi
// count" + "A reg value ..." + "V x y z w a ..." (positions, ITOF0 as VU1 reads them, and the colour's alpha) + "T i j k ...";
// per draw "D kind id0 id1 id2 m[16] meshes...", and the adapter's notes (rd_dump_note). Off: nothing.
struct Dump {
    FILE *f = nullptr;
    bool checked = false;
    uint64_t parsed = 0, reparsed = 0, hashed = 0, hash_ns = 0, parse_ns = 0;
    std::unordered_map<RdMesh, bool> written;
    FILE *file() {
        if (!checked) {
            checked = true;
            const char *path = getenv("KF4_ND_DUMP");
            if (path && *path) {
                f = fopen(path, "w");
            }
        }
        return f;
    }
} dump;

void dump_mesh(const RdMeshData &m) {
    FILE *f = dump.f;
    fprintf(f, "M %u %#x %zu\n", m.id, m.key, m.batches.size());
    for (const RdBatch &b : m.batches) {
        fprintf(f, "B %#x %#llx %u %u %u\n", b.start, (unsigned long long)((b.tag[0] >> 47) & 0x7FF), b.alpha_lo,
                b.alpha_hi, b.count);
        fprintf(f, "A");
        for (uint64_t v : b.ad) {
            fprintf(f, " %#llx", (unsigned long long)v);
        }
        fprintf(f, "\nV");
        for (uint32_t k = 0; k < b.count; k++) {
            const int32_t *v = &m.vertices[size_t(b.first + k) * rd_vertex_words(m.format)];
            fprintf(f, " %d %d %d %d %d", v[0], v[1], v[2], v[3], v[m.format == RD_VF_GS_STRIP ? 3 : 15]);
        }
        fprintf(f, "\nT");
        for (uint8_t t : b.triangles) {
            fprintf(f, " %u", t);
        }
        fprintf(f, "\n");
    }
}

// ---- The side buffer ----------------------------------------------------------------------------
const uint32_t SLOTS = 128; // (a power of two: the 24-bit serial wraps onto the same slot)
struct Slot {
    uint32_t serial = 0;
    std::vector<RdDraw> draws;
    std::vector<uint32_t> first_mesh; // each draw's first in meshes
    std::vector<std::shared_ptr<const RdMeshData>> meshes;
    std::vector<RdCamera> cameras;
};
std::mutex slots_lock; // (the game's thread writes the slot of its frame; the GS's reads older ones)
Slot slots[SLOTS];
uint32_t serial = 0; // the frame's (24 bits in a handle)
uint32_t frame_count = 0;
// the consumer (the PC renderer): the last frame whose RD_ND_FRAME mark it has met (rd_consumer_frame)
std::atomic<uint32_t> consumer_frame{0};
std::atomic<bool> consumer{false};
std::atomic<uint64_t> drawn{0}; // rd_draw's calls (rd_capture_poll looks for new ones)

} // namespace

extern "C" RdMesh rd_mesh_from_block(const void *block, uint32_t qwords, uint32_t key, uint32_t gen, RdVertexFormat fmt) {
    const uint8_t *bytes = static_cast<const uint8_t *>(block);
    uint32_t want = gen;
    if (gen == RD_GEN_HASH) {
        auto t0 = dump.file() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        uint64_t h = hash_bytes(bytes, size_t(qwords) * 16);
        want = uint32_t(h ^ (h >> 32)) | 1u;
        rd_stats.hash_bytes += uint64_t(qwords) * 16;
        if (dump.f) { // (KF4_ND_DUMP)
            dump.hash_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            dump.hashed += uint64_t(qwords) * 16;
        }
    }
    auto it = cache.find(key);
    bool hit = it != cache.end() && it->second.gen == want && !it->second.touched && it->second.format == uint32_t(fmt);
    if (dump.f && !hit) { // (KF4_ND_DUMP)
        dump.parsed++;
        if (it != cache.end()) {
            dump.reparsed++;
            fprintf(dump.f, "P %#x %u\n", key, qwords);
        }
    }
    if (hit) {
        it->second.frame = frame_count;
        rd_stats.mesh_hits++;
        if (!it->second.mesh) {
            why_refused = "refused before";
            return 0;
        }
        return it->second.mesh->id;
    }
    auto mesh = std::make_shared<RdMeshData>();
    mesh->key = key;
    mesh->gen = want;
    rd_stats.meshes_parsed++;
    auto p0 = dump.f ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    bool ok = (fmt == RD_VF_PACKED16 || fmt == RD_VF_FLOAT_SKIN || fmt == RD_VF_GS_STRIP || fmt == RD_VF_SILHOUETTE) &&
              parse_block(bytes, qwords, *mesh, fmt);
    if (dump.f) { // (KF4_ND_DUMP)
        dump.parse_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - p0).count());
    }
    if (fmt != RD_VF_PACKED16 && fmt != RD_VF_FLOAT_SKIN && fmt != RD_VF_GS_STRIP && fmt != RD_VF_SILHOUETTE) {
        why_refused = "not a vertex format the parser knows";
    }
    CacheEntry &e = cache[key];
    if (e.mesh) {
        by_id.erase(e.mesh->id);
    }
    e.gen = want;
    e.touched = 0;
    e.frame = frame_count;
    e.format = uint32_t(fmt);
    if (!ok) {
        e.mesh.reset();
        rd_stats.mesh_unsupported++;
        static const char *log = getenv("KF4_ND_LOG");
        if (log) {
            fprintf(stderr, "[nd] block %#x (%u qwords) refused: %s\n", key, qwords, why_refused);
        }
        return 0;
    }
    mesh->block.assign(bytes, bytes + size_t(qwords) * 16);
    mesh->format = uint32_t(fmt);
    mesh->id = next_id++;
    if (next_id == 0) {
        next_id = 1;
    }
    e.mesh = mesh;
    by_id[mesh->id] = mesh;
    rd_stats.meshes_held = by_id.size();
    return mesh->id;
}

extern "C" int rd_dump_enabled(void) {
    return dump.file() != nullptr;
}

extern "C" void rd_dump_note(const char *line) {
    if (dump.file()) {
        fprintf(dump.f, "%s\n", line);
    }
}

extern "C" void rd_mesh_touch(uint32_t key) {
    auto it = cache.find(key);
    if (it != cache.end()) {
        it->second.touched = 1;
    }
}

extern "C" const char *rd_mesh_why(void) {
    return why_refused;
}

bool rd_batch_280_from_vu(const uint32_t (*mem)[4], uint32_t top, uint32_t start, RdMeshData &mesh) {
    static VuModel vu; // (kf4-gsreplay's thread only)
    memcpy(vu.mem, mem, sizeof(vu.mem));
    memset(vu.written, 1, sizeof(vu.written));
    mesh.format = RD_VF_FLOAT_SKIN;
    return take_batch_280(vu, top % VU_QWORDS, start, mesh);
}

bool rd_batch_from_vu(const uint32_t (*mem)[4], uint32_t top, uint32_t start, RdMeshData &mesh) {
    static VuModel vu; // (kf4-gsreplay's thread only)
    memcpy(vu.mem, mem, sizeof(vu.mem));
    memset(vu.written, 1, sizeof(vu.written));
    memset(vu.ever, 1, sizeof(vu.ever));
    mesh.format = RD_VF_PACKED16;
    return take_batch(vu, top % VU_QWORDS, start, mesh);
}

bool rd_batch_5a0_from_vu(const uint32_t (*mem)[4], uint32_t top, uint32_t start, RdMeshData &mesh) {
    static VuModel vu; // (kf4-gsreplay's thread only)
    memcpy(vu.mem, mem, sizeof(vu.mem));
    memset(vu.written, 1, sizeof(vu.written));
    memset(vu.ever, 1, sizeof(vu.ever));
    mesh.format = RD_VF_SILHOUETTE;
    return take_batch(vu, top % VU_QWORDS, start, mesh, true);
}

extern "C" int rd_mesh_runs_program(RdMesh mesh, uint32_t program) {
    auto it = by_id.find(mesh);
    if (it == by_id.end()) {
        return 0;
    }
    for (const RdBatch &b : it->second->batches) {
        if (b.start != RD_PROGRAM_CONTINUE && b.start != program) {
            return 0;
        }
    }
    return 1;
}

bool rd_parse_block(const void *block, uint32_t qwords, RdVertexFormat fmt, RdMeshData &mesh) {
    mesh.format = uint32_t(fmt);
    return parse_block(static_cast<const uint8_t *>(block), qwords, mesh, fmt);
}

extern "C" uint32_t rd_mesh_program(RdMesh mesh) {
    auto it = by_id.find(mesh);
    return it == by_id.end() || it->second->batches.empty() ? RD_PROGRAM_CONTINUE : it->second->batches[0].start;
}

extern "C" uint64_t rd_begin_frame(void) {
    // The slot this frame takes was a frame's SLOTS ago: if the consumer has not got that far (its
    // pictures between are drawn a frame after it), wait for it. (The GS's ring of 64 MB holds some 50
    // frames of play: this is a guard that should not be met. Not for ever: after 2 s the draws go.)
    uint32_t next = (serial + 1) & 0xFFFFFF;
    if (consumer.load() && !slots[next % SLOTS].draws.empty()) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (((consumer_frame.load() - slots[next % SLOTS].serial) & 0xFFFFFF) < 2 ||
               ((consumer_frame.load() - slots[next % SLOTS].serial) & 0x800000)) {
            if (std::chrono::steady_clock::now() > until) {
                rd_stats.waits_given_up++;
                break;
            }
            rd_stats.waits++;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    std::lock_guard<std::mutex> g(slots_lock);
    serial = next;
    frame_count++;
    if (dump.file()) { // (KF4_ND_DUMP: the frame before's counts, then this frame's serial)
        fprintf(dump.f, "F %u %llu %llu %llu %llu %llu\n", serial, (unsigned long long)dump.parsed,
                (unsigned long long)dump.reparsed, (unsigned long long)dump.hashed, (unsigned long long)dump.hash_ns,
                (unsigned long long)dump.parse_ns);
        dump.parsed = dump.reparsed = dump.hashed = dump.hash_ns = dump.parse_ns = 0;
        fflush(dump.f);
    }
    Slot &s = slots[serial % SLOTS];
    s.serial = serial;
    s.draws.clear();
    s.first_mesh.clear();
    s.meshes.clear();
    s.cameras.clear();
    // meshes not asked for in a while leave the cache (a slot that still names one keeps it alive)
    if (frame_count % 64 == 0) {
        for (auto it = cache.begin(); it != cache.end();) {
            if (frame_count - it->second.frame > 600) {
                if (it->second.mesh) {
                    by_id.erase(it->second.mesh->id);
                }
                it = cache.erase(it);
            } else {
                ++it;
            }
        }
        rd_stats.meshes_held = by_id.size();
    }
    return (uint64_t(RD_ND_FRAME) << 56) | serial;
}

extern "C" void rd_consumer_frame(uint64_t value) {
    consumer_frame.store(uint32_t(value) & 0xFFFFFF);
    consumer.store(true);
}

extern "C" uint32_t rd_camera(const RdCamera *c) {
    std::lock_guard<std::mutex> g(slots_lock);
    Slot &s = slots[serial % SLOTS];
    if (!s.cameras.empty() && memcmp(&s.cameras.back(), c, sizeof(*c)) == 0) {
        return uint32_t(s.cameras.size() - 1);
    }
    s.cameras.push_back(*c);
    return uint32_t(s.cameras.size() - 1);
}

extern "C" uint64_t rd_draw(const RdDraw *d, const RdMesh *meshes, uint32_t count) {
    std::lock_guard<std::mutex> g(slots_lock);
    Slot &s = slots[serial % SLOTS];
    if (d->camera >= s.cameras.size()) {
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (by_id.find(meshes[i]) == by_id.end()) {
            return 0;
        }
    }
    uint32_t index = uint32_t(s.draws.size());
    s.draws.push_back(*d);
    s.draws.back().mesh_count = count;
    s.first_mesh.push_back(uint32_t(s.meshes.size()));
    for (uint32_t i = 0; i < count; i++) {
        s.meshes.push_back(by_id[meshes[i]]);
    }
    rd_stats.draws++;
    if (dump.file()) { // (KF4_ND_DUMP)
        for (uint32_t i = 0; i < count; i++) {
            if (!dump.written[meshes[i]]) {
                dump.written[meshes[i]] = true;
                dump_mesh(*by_id[meshes[i]]);
            }
        }
        fprintf(dump.f, "D %u %#x %u %u", d->kind, d->id[0], d->id[1], d->id[2]);
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                fprintf(dump.f, " %.9g", d->model[c][r]);
            }
        }
        for (uint32_t i = 0; i < count; i++) {
            fprintf(dump.f, " %u", meshes[i]);
        }
        fprintf(dump.f, "\n");
    }
    drawn.fetch_add(1, std::memory_order_release);
    return (uint64_t(RD_ND_DRAW) << 56) | (uint64_t(serial) << 32) | index;
}

// A pass's words in a draw (rd_pass, rd_pass_of): its first twelve in colour, its data in the six
// matrices a capture keeps (model, parent, light_dir, light_col, point_pos, point_col), so that a capture
// carries passes as it carries draws (gs_capture_format.h's Kf4GsNdDraw)
static_assert(sizeof(RdPass) == (12 + 96) * 4, "RdPass is twelve words and its data");
static void pass_words(RdDraw &d, RdPass &p, bool to_draw) {
    uint32_t *head = reinterpret_cast<uint32_t *>(&p);
    float *parts[6] = {&d.model[0][0], &d.parent[0][0], &d.light_dir[0][0], &d.light_col[0][0], &d.point_pos[0][0],
                       &d.point_col[0][0]};
    if (to_draw) {
        memcpy(&d.colour[0][0], head, 12 * 4);
        for (int i = 0; i < 6; i++) {
            memcpy(parts[i], p.data + i * 16, 64);
        }
    } else {
        memcpy(head, &d.colour[0][0], 12 * 4);
        for (int i = 0; i < 6; i++) {
            memcpy(p.data + i * 16, parts[i], 64);
        }
    }
}

void rd_pass_of(const RdDraw &d, RdPass &p) {
    pass_words(const_cast<RdDraw &>(d), p, false);
}

extern "C" uint64_t rd_pass(const RdPass *p) {
    static uint32_t counted_serial = 0xFFFFFFFFu;
    static uint32_t counts[16];
    RdDraw d;
    memset(&d, 0, sizeof(d));
    d.kind = RD_KIND_PASS;
    d.program = RD_PROGRAM_PASS;
    pass_words(d, const_cast<RdPass &>(*p), true);
    std::lock_guard<std::mutex> g(slots_lock);
    Slot &s = slots[serial % SLOTS];
    if (counted_serial != serial) {
        counted_serial = serial;
        memset(counts, 0, sizeof(counts));
    }
    // (its identity: the pass, its call site, and the call site's count in the frame)
    d.id[0] = p->pass;
    d.id[1] = p->site;
    d.id[2] = counts[p->site & 15]++;
    if (s.cameras.empty()) {
        s.cameras.push_back(RdCamera{}); // (a pass has no camera; rd_fetch takes one all the same)
    }
    d.camera = 0;
    uint32_t index = uint32_t(s.draws.size());
    s.draws.push_back(d);
    s.first_mesh.push_back(uint32_t(s.meshes.size()));
    rd_stats.draws++;
    rd_stats.passes++;
    drawn.fetch_add(1, std::memory_order_release);
    return (uint64_t(RD_ND_DRAW) << 56) | (uint64_t(serial) << 32) | index;
}

bool rd_fetch(uint64_t value, RdDraw &draw, RdCamera &camera, std::vector<std::shared_ptr<const RdMeshData>> &meshes) {
    uint32_t want = uint32_t(value >> 32) & 0xFFFFFF, index = uint32_t(value);
    std::lock_guard<std::mutex> g(slots_lock);
    const Slot &s = slots[want % SLOTS];
    if (s.serial != want || index >= s.draws.size()) {
        rd_stats.lost++;
        return false;
    }
    draw = s.draws[index];
    camera = s.cameras[draw.camera];
    meshes.assign(s.meshes.begin() + s.first_mesh[index], s.meshes.begin() + s.first_mesh[index] + draw.mesh_count);
    return true;
}

// ---- Captures (render.h, gs_capture_format.h) -----------------------------------------------------
static_assert(sizeof(Kf4GsNdDraw) + sizeof(RdDraw::turn) == sizeof(RdDraw),
              "Kf4GsNdDraw is RdDraw's fields, in order, but the turns (after the mesh ids, for 0x280 only)");
static_assert(sizeof(Kf4GsNdCamera) == sizeof(RdCamera), "Kf4GsNdCamera is RdCamera's fields, in order");

namespace {

// (field by field: the file's layout is its own, whatever RdDraw becomes)
void draw_to_file(const RdDraw &d, Kf4GsNdDraw &f) {
    memset(&f, 0, sizeof(f));
    f.kind = d.kind;
    memcpy(f.id, d.id, sizeof(f.id));
    f.program = d.program;
    f.flags = d.flags;
    f.camera = d.camera;
    f.mesh_count = d.mesh_count;
    memcpy(f.model, d.model, sizeof(f.model));
    memcpy(f.parent, d.parent, sizeof(f.parent));
    memcpy(f.light_dir, d.light_dir, sizeof(f.light_dir));
    memcpy(f.light_col, d.light_col, sizeof(f.light_col));
    memcpy(f.fog, d.fog, sizeof(f.fog));
    f.fogcol = d.fogcol;
    f.point_count = d.point_count;
    memcpy(f.point_pos, d.point_pos, sizeof(f.point_pos));
    memcpy(f.point_col, d.point_col, sizeof(f.point_col));
    memcpy(f.colour, d.colour, sizeof(f.colour));
}

void draw_from_file(const Kf4GsNdDraw &f, RdDraw &d) {
    memset(&d, 0, sizeof(d));
    d.kind = f.kind;
    memcpy(d.id, f.id, sizeof(d.id));
    d.program = f.program;
    d.flags = f.flags;
    d.camera = f.camera;
    d.mesh_count = f.mesh_count;
    memcpy(d.model, f.model, sizeof(d.model));
    memcpy(d.parent, f.parent, sizeof(d.parent));
    memcpy(d.light_dir, f.light_dir, sizeof(d.light_dir));
    memcpy(d.light_col, f.light_col, sizeof(d.light_col));
    memcpy(d.fog, f.fog, sizeof(d.fog));
    d.fogcol = f.fogcol;
    d.point_count = f.point_count;
    memcpy(d.point_pos, f.point_pos, sizeof(d.point_pos));
    memcpy(d.point_col, f.point_col, sizeof(d.point_col));
    memcpy(d.colour, f.colour, sizeof(d.colour));
}

void camera_to_file(const RdCamera &c, Kf4GsNdCamera &f) {
    memcpy(f.world_screen, c.world_screen, sizeof(f.world_screen));
    memcpy(f.view, c.view, sizeof(f.view));
    memcpy(f.screen, c.screen, sizeof(f.screen));
    f.mode = c.mode;
    f.projection = c.projection;
}

void camera_from_file(const Kf4GsNdCamera &f, RdCamera &c) {
    memcpy(c.world_screen, f.world_screen, sizeof(c.world_screen));
    memcpy(c.view, f.view, sizeof(c.view));
    memcpy(c.screen, f.screen, sizeof(c.screen));
    c.mode = f.mode;
    c.projection = f.projection;
}

template <typename T> void append(std::vector<uint8_t> &body, const T &v) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
    body.insert(body.end(), p, p + sizeof(T));
}

// The capture's side (its thread: the DMA controller's)
struct CaptureState {
    bool fresh = true;   // rd_capture_begin: the next poll gives the frames the GS may not have met
    uint64_t drawn_seen = 0;
    uint32_t cursor = 0; // the oldest frame whose draws may still grow
    std::unordered_map<uint32_t, uint32_t> given; // frame serial: its draws given so far
    std::unordered_set<RdMesh> meshes_given;
    std::vector<uint8_t> body;
} capture;

void capture_frame(const Slot &s, RdCaptureEmit emit) {
    uint32_t &given = capture.given[s.serial];
    uint32_t count = uint32_t(s.draws.size());

    if (count <= given) {
        return;
    }
    // the meshes first: a reader has them when the draws name them
    for (uint32_t i = given; i < count; i++) {
        for (uint32_t m = 0; m < s.draws[i].mesh_count; m++) {
            const RdMeshData &mesh = *s.meshes[s.first_mesh[i] + m];
            if (capture.meshes_given.insert(mesh.id).second) {
                Kf4GsNdMesh head;
                head.id = mesh.id;
                head.key = mesh.key;
                head.qwords = uint32_t(mesh.block.size() / 16);
                head.format = mesh.format;
                emit(KF4GS_ND_MESH, &head, sizeof(head), mesh.block.data(), mesh.block.size());
            }
        }
    }
    Kf4GsNdDraws head;
    head.serial = s.serial;
    head.first = given;
    head.count = count - given;
    head.cameras = uint32_t(s.cameras.size());
    std::vector<uint8_t> &body = capture.body;
    body.clear();
    for (const RdCamera &c : s.cameras) {
        Kf4GsNdCamera f;
        camera_to_file(c, f);
        append(body, f);
    }
    for (uint32_t i = given; i < count; i++) {
        Kf4GsNdDraw f;
        draw_to_file(s.draws[i], f);
        append(body, f);
        for (uint32_t m = 0; m < s.draws[i].mesh_count; m++) {
            append(body, uint32_t(s.meshes[s.first_mesh[i] + m]->id));
        }
        if (s.draws[i].program == 0x280) {
            append(body, s.draws[i].turn); // (gs_capture_format.h: only a 0x280 draw's)
        }
    }
    emit(KF4GS_ND_DRAWS, &head, sizeof(head), body.data(), body.size());
    given = count;
}

// The replay's side (kf4-gsreplay's thread): the capture's meshes by its ids
std::unordered_map<uint32_t, std::shared_ptr<const RdMeshData>> replay_meshes;

} // namespace

RdReplayStats rd_replay_stats;

extern "C" void rd_capture_begin(void) {
    capture.fresh = true;
    capture.given.clear();
    capture.meshes_given.clear();
}

extern "C" void rd_capture_poll(RdCaptureEmit emit) {
    uint64_t n = drawn.load(std::memory_order_acquire);
    if (!capture.fresh && n == capture.drawn_seen) {
        return;
    }
    std::lock_guard<std::mutex> g(slots_lock);
    uint32_t now = serial;
    if (capture.fresh) {
        // what the chain on its way to the GS can still name: the frames from the consumer's last mark
        // on (it is behind the DMA), or the last few when there is no consumer
        uint32_t back = consumer.load() ? ((now - consumer_frame.load()) & 0xFFFFFF) : 3;
        back = std::min(back, SLOTS - 2);
        capture.cursor = (now - back) & 0xFFFFFF;
        capture.fresh = false;
    }
    capture.drawn_seen = n;
    for (uint32_t s = capture.cursor;; s = (s + 1) & 0xFFFFFF) {
        const Slot &slot = slots[s % SLOTS];
        if (slot.serial == s) {
            capture_frame(slot, emit);
        }
        if (s == now) {
            break;
        }
    }
    capture.cursor = now; // (rd_draw adds to the newest frame only)
}

extern "C" void rd_replay_reset(void) {
    replay_meshes.clear();
    memset(&rd_replay_stats, 0, sizeof(rd_replay_stats));
}

extern "C" int rd_replay_record(uint32_t type, const void *data, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    if (type == KF4GS_ND_MESH) {
        Kf4GsNdMesh head;
        if (size < sizeof(head)) {
            return -1;
        }
        memcpy(&head, p, sizeof(head));
        if (size - sizeof(head) < size_t(head.qwords) * 16) {
            return -1;
        }
        auto mesh = std::make_shared<RdMeshData>();
        mesh->id = head.id;
        mesh->key = head.key;
        mesh->gen = 0;
        mesh->format = head.format;
        mesh->block.assign(p + sizeof(head), p + sizeof(head) + size_t(head.qwords) * 16);
        rd_replay_stats.meshes++;
        // (parsed again: this build's parser decides, as it would in the game)
        bool known = head.format == RD_VF_PACKED16 || head.format == RD_VF_FLOAT_SKIN || head.format == RD_VF_GS_STRIP ||
                     head.format == RD_VF_SILHOUETTE;
        if (!known || !parse_block(mesh->block.data(), head.qwords, *mesh, RdVertexFormat(head.format))) {
            rd_replay_stats.meshes_refused++;
            fprintf(stderr, "native draw: the capture's block %#x (%u qwords) is refused: %s\n", head.key, head.qwords,
                    !known ? "not a vertex format the parser knows" : why_refused);
            replay_meshes.erase(head.id);
            return 1;
        }
        replay_meshes[head.id] = mesh;
        return 1;
    }
    if (type != KF4GS_ND_DRAWS) {
        return 0;
    }
    Kf4GsNdDraws head;
    if (size < sizeof(head)) {
        return -1;
    }
    memcpy(&head, p, sizeof(head));
    size_t at = sizeof(head);
    if ((size - at) / sizeof(Kf4GsNdCamera) < head.cameras) {
        return -1;
    }
    std::lock_guard<std::mutex> g(slots_lock);
    Slot &s = slots[head.serial % SLOTS];
    if (s.serial != head.serial || head.first == 0) {
        s.serial = head.serial;
        s.draws.clear();
        s.first_mesh.clear();
        s.meshes.clear();
    }
    s.cameras.clear();
    for (uint32_t i = 0; i < head.cameras; i++, at += sizeof(Kf4GsNdCamera)) {
        Kf4GsNdCamera f;
        RdCamera c;
        memcpy(&f, p + at, sizeof(f));
        memset(&c, 0, sizeof(c));
        camera_from_file(f, c);
        s.cameras.push_back(c);
    }
    if (s.cameras.empty()) {
        s.cameras.push_back(RdCamera{}); // (a draw left out still names one)
    }
    // the frame's draws from first on are these (one the capture did not give before them: left out)
    if (s.draws.size() > head.first) {
        s.meshes.resize(s.first_mesh[head.first]);
        s.draws.resize(head.first);
        s.first_mesh.resize(head.first);
    }
    RdDraw none; // (a draw left out: the renderer counts it as of a program it does not draw)
    memset(&none, 0, sizeof(none));
    none.program = 0xFFFFFFFFu;
    while (s.draws.size() < head.first) {
        s.draws.push_back(none);
        s.first_mesh.push_back(uint32_t(s.meshes.size()));
        rd_replay_stats.draws_dropped++;
    }
    for (uint32_t i = 0; i < head.count; i++) {
        Kf4GsNdDraw f;
        RdDraw d;
        if (size - at < sizeof(f)) {
            return -1;
        }
        memcpy(&f, p + at, sizeof(f));
        at += sizeof(f);
        draw_from_file(f, d);
        if ((size - at) / 4 < d.mesh_count) {
            return -1;
        }
        bool ok = d.camera < s.cameras.size();
        size_t first_mesh = s.meshes.size();
        for (uint32_t m = 0; m < d.mesh_count; m++, at += 4) {
            uint32_t id;
            memcpy(&id, p + at, 4);
            auto it = replay_meshes.find(id);
            if (it == replay_meshes.end()) {
                ok = false;
            } else if (ok) {
                s.meshes.push_back(it->second);
            }
        }
        if (d.program == 0x280) {
            if (size - at < sizeof(d.turn)) {
                return -1;
            }
            memcpy(d.turn, p + at, sizeof(d.turn));
            at += sizeof(d.turn);
        }
        if (!ok) {
            s.meshes.resize(first_mesh); // (drawn as nothing: the frame's indices stay the capture's)
            d = none;
            rd_replay_stats.draws_dropped++;
        } else {
            rd_replay_stats.draws++;
        }
        s.first_mesh.push_back(uint32_t(first_mesh));
        s.draws.push_back(d);
    }
    return 1;
}
