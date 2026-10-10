#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_ref.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

void vu_verify_normalize_candidate(const float *in, float *out);
void vu_verify_normalize_reference(const float *in, float *out);
void vu_verify_classify_candidate(const double *in, uint8_t *out);
void vu_verify_classify_reference(const double *in, uint8_t *out);

namespace {
constexpr uint32_t nop = 0x2ffu;
constexpr uint32_t ebit = 0x40000000u;
using Packets = std::vector<std::vector<uint8_t>>;
uint64_t checks = 0, calls = 0;
uint64_t candidateNanoseconds = 0, referenceNanoseconds = 0;
std::string control;
bool controlInjected = false;
uint32_t upper(uint32_t op, uint32_t mask = 15, uint32_t ft = 3, uint32_t fs = 2, uint32_t fd = 4) {
    return op | (mask << 21) | (ft << 16) | (fs << 11) | (fd << 6);
}
uint32_t special(uint32_t selector, uint32_t mask = 0, uint32_t ft = 0, uint32_t fs = 0) {
    return 0x3cu | (selector & 3u) | ((selector & 0x7cu) << 4) | (mask << 21) | (ft << 16) | (fs << 11);
}
uint32_t lowerSpecial(uint32_t selector, uint32_t is, uint32_t it = 0, uint32_t mask = 0) {
    return 0x80000000u | special(selector, mask, it, is);
}
uint64_t tag(uint32_t loops, uint32_t format, uint32_t regs, bool end = true) {
    return loops | (end ? 0x8000ull : 0) | (uint64_t(format) << 58) | (uint64_t(regs) << 60);
}
struct Pair {
    std::unique_ptr<PS2Memory> cm = std::make_unique<PS2Memory>();
    std::unique_ptr<PS2Memory> rm = std::make_unique<PS2Memory>();
    std::unique_ptr<VU1Interpreter> c = std::make_unique<VU1Interpreter>();
    std::unique_ptr<VU1InterpreterRef> r = std::make_unique<VU1InterpreterRef>();
    GS gs;
    Packets cp, rp;
    Pair() {
        if (!cm->initialize() || !rm->initialize()) throw std::runtime_error("memory initialization");
        cm->setGifPacketCallback([this](const uint8_t *data, uint32_t bytes) { cp.emplace_back(data, data + bytes); });
        rm->setGifPacketCallback([this](const uint8_t *data, uint32_t bytes) { rp.emplace_back(data, data + bytes); });
    }
    void reset() {
        c->reset(); r->reset(); cp.clear(); rp.clear();
        std::memset(cm->getVU1Code(), 0, PS2_VU1_CODE_SIZE);
        std::memset(rm->getVU1Code(), 0, PS2_VU1_CODE_SIZE);
        std::memset(cm->getVU1Data(), 0, PS2_VU1_DATA_SIZE);
        std::memset(rm->getVU1Data(), 0, PS2_VU1_DATA_SIZE);
    }
    void instruction(uint32_t pc, uint32_t lo, uint32_t hi) {
        const uint64_t pair = lo | (uint64_t(hi) << 32);
        cm->write64(PS2_VU1_CODE_BASE + pc, pair);
        rm->write64(PS2_VU1_CODE_BASE + pc, pair);
    }
    void seed(uint32_t salt = 1) {
        std::mt19937 random(salt);
        static constexpr uint32_t values[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x00000001u,
            0x80000001u, 0x00800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u, 0x7fc12345u,
            0xffc12345u, 0x3f000000u, 0x3f7fffffu, 0x4b800001u, 0xcf000000u};
        auto &state = c->state();
        for (auto &vf : state.vf) for (float &lane : vf) lane = std::bit_cast<float>(values[random() % std::size(values)]);
        for (float &lane : state.acc) lane = std::bit_cast<float>(values[random() % std::size(values)]);
        state.q = 0.75f; state.p = 0.25f; state.i = -0.5f;
        state.mac = 0x1234; state.status = 0xf81; state.clip = 0x543210;
        state.top = 0x30; state.itop = 0x51;
        std::memcpy(&r->state(), &state, sizeof(state));
    }
    void data(uint32_t offset, const void *bytes, uint32_t size) {
        std::memcpy(cm->getVU1Data() + offset, bytes, size);
        std::memcpy(rm->getVU1Data() + offset, bytes, size);
    }
    void packet(uint32_t offset, uint32_t loops, uint32_t format, uint32_t regs, uint8_t value, bool end = true) {
        std::array<uint8_t, 16384> bytes{};
        const uint64_t lo = tag(loops, format, regs, end);
        std::memcpy(bytes.data(), &lo, 8);
        uint32_t length = 16;
        if (format == 0) length += loops * (regs ? regs : 16) * 16;
        if (format == 1) length += ((loops * (regs ? regs : 16) + 1) & ~1u) * 8;
        if (format == 2) length += loops * 16;
        if (length > bytes.size()) throw std::runtime_error("synthetic packet too large");
        std::fill(bytes.begin() + 16, bytes.begin() + length, value);
        for (uint32_t i = 0; i < length; ++i) {
            cm->getVU1Data()[(offset + i) & 0x3fff] = bytes[i];
            rm->getVU1Data()[(offset + i) & 0x3fff] = bytes[i];
        }
    }
    void check(bool ok, const std::string &name) {
        ++checks;
        if (!ok) throw std::runtime_error("MISMATCH call=" + std::to_string(calls) + " category=" + name);
    }
    void compare() {
        if (!control.empty() && !controlInjected && !cp.empty()) {
            controlInjected = true;
            if (control == "registers") c->state().vi[9] ^= 1;
            else if (control == "flags") c->state().status ^= 1;
            else if (control == "cycles") ++c->state().cycles;
            else if (control == "memory") cm->getVU1Data()[123] ^= 1;
            else if (control == "packet") cp[0][0] ^= 1;
        }
        auto &a = c->state(); auto &b = r->state();
        check(std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
            std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, 12) == 0 && a.r == b.r, "registers");
        check(a.mac == b.mac && a.clip == b.clip && a.status == b.status, "flags");
        check(a.cycles == b.cycles, "cycles");
        check(a.pc == b.pc && a.ebit == b.ebit && a.haltAfterDelaySlot == b.haltAfterDelaySlot &&
            a.dBitEnabled == b.dBitEnabled && a.tBitEnabled == b.tBitEnabled && a.stoppedByD == b.stoppedByD &&
            a.stoppedByT == b.stoppedByT && a.top == b.top && a.itop == b.itop && a.branchPending == b.branchPending &&
            a.branchTarget == b.branchTarget && a.branchDelay == b.branchDelay, "control-state");
        check(std::memcmp(cm->getVU1Data(), rm->getVU1Data(), PS2_VU1_DATA_SIZE) == 0, "memory");
        check(cp == rp, "packet");
    }
    void run(bool resume = false, uint32_t budget = 65536, uint32_t start = 0, uint32_t top = 0, uint32_t itop = 0) {
        cp.clear(); rp.clear(); ++calls;
        auto candidate = [&] {
            if (resume) c->resume(cm->getVU1Code(), PS2_VU1_CODE_SIZE, cm->getVU1Data(), PS2_VU1_DATA_SIZE, gs, cm.get(), top, itop, budget);
            else c->execute(cm->getVU1Code(), PS2_VU1_CODE_SIZE, cm->getVU1Data(), PS2_VU1_DATA_SIZE, gs, cm.get(), start, top, itop, budget);
        };
        auto reference = [&] {
            if (resume) r->resume(rm->getVU1Code(), PS2_VU1_CODE_SIZE, rm->getVU1Data(), PS2_VU1_DATA_SIZE, gs, rm.get(), top, itop, budget);
            else r->execute(rm->getVU1Code(), PS2_VU1_CODE_SIZE, rm->getVU1Data(), PS2_VU1_DATA_SIZE, gs, rm.get(), start, top, itop, budget);
        };
        const auto startTime = std::chrono::steady_clock::now();
        if (calls & 1) candidate(); else reference();
        const auto middleTime = std::chrono::steady_clock::now();
        if (calls & 1) reference(); else candidate();
        const auto endTime = std::chrono::steady_clock::now();
        const uint64_t first = std::chrono::duration_cast<std::chrono::nanoseconds>(middleTime - startTime).count();
        const uint64_t second = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - middleTime).count();
        candidateNanoseconds += (calls & 1) ? first : second;
        referenceNanoseconds += (calls & 1) ? second : first;
        compare();
    }
};
void synthetic() {
    Pair p;
    // Direct four-lane normalization compares the real SIMD and scalar helpers
    // across random IEEE encodings, exponent boundaries and both signs.
    std::mt19937 bits(0x39aabbcc);
    for (uint32_t sample = 0; sample < 250000; ++sample) {
        float input[4], candidate[4], reference[4];
        for (auto &lane : input) lane = std::bit_cast<float>(bits());
        vu_verify_normalize_candidate(input, candidate);
        vu_verify_normalize_reference(input, reference);
        p.check(std::memcmp(candidate, reference, sizeof(candidate)) == 0, "normalization-random-bits");
    }
    for (uint32_t exponent = 0; exponent < 256; ++exponent) for (uint32_t mantissa : {0u,1u,0x3fffffu,0x7fffffu}) {
        const uint32_t value = (exponent << 23) | mantissa;
        float input[4] = {std::bit_cast<float>(value),std::bit_cast<float>(value | 0x80000000u),
            std::bit_cast<float>(value ^ 0x400000u),std::bit_cast<float>((value ^ 0x400000u) | 0x80000000u)};
        float candidate[4], reference[4];
        vu_verify_normalize_candidate(input, candidate); vu_verify_normalize_reference(input, reference);
        p.check(std::memcmp(candidate, reference, sizeof(candidate)) == 0, "normalization-boundaries");
    }
    // Direct exact double classification across arbitrary IEEE encodings,
    // NaN signs/payloads and boundaries around zero, FLT_MIN and FLT_MAX.
    for (uint32_t sample = 0; sample < 250000; ++sample) {
        double input[2]; uint8_t candidate[2], reference[2];
        for (auto &lane : input) lane = std::bit_cast<double>((uint64_t(bits()) << 32u) | bits());
        vu_verify_classify_candidate(input, candidate); vu_verify_classify_reference(input, reference);
        p.check(std::memcmp(candidate, reference, sizeof(candidate)) == 0, "double-classification-random-bits");
    }
    for (double boundary : {0.0, static_cast<double>(std::numeric_limits<float>::min()), static_cast<double>(std::numeric_limits<float>::max())}) {
        const uint64_t encoding = std::bit_cast<uint64_t>(boundary);
        for (int offset = -4; offset <= 4; ++offset) {
            const uint64_t value = encoding + offset;
            double input[2] = {std::bit_cast<double>(value), std::bit_cast<double>(value | 0x8000000000000000ull)};
            uint8_t candidate[2], reference[2];
            vu_verify_classify_candidate(input, candidate); vu_verify_classify_reference(input, reference);
            p.check(std::memcmp(candidate, reference, sizeof(candidate)) == 0, "double-classification-boundaries");
        }
    }
    // NOP selectors and encoded register/destination fields, including I/E/D/T bits.
    for (uint32_t selector : {0x2fu, 0x30u}) for (uint32_t mask = 0; mask < 16; ++mask) for (uint32_t flags = 0; flags < 16; ++flags) {
        p.reset(); p.seed(selector * 1000 + mask * 16 + flags);
        p.c->state().dBitEnabled = p.r->state().dBitEnabled = flags & 1;
        p.c->state().tBitEnabled = p.r->state().tBitEnabled = flags & 2;
        const uint32_t bits = ((flags & 1) ? 0x80000000u : 0) | ((flags & 2) ? 0x10000000u : 0) |
            ((flags & 4) ? 0x08000000u : 0) | ((flags & 8) ? ebit : 0);
        p.instruction(0, (bits & 0x80000000u) ? 0x3f7fffffu : 0u, special(selector, mask, 3, 2) | bits);
        p.instruction(8, 0, nop | ebit); p.instruction(16, 0, nop);
        p.run(false, 4); p.run(true, 4);
    }
    // Every normal FMAC opcode and all masks with difficult scalar operands.
    constexpr uint32_t ops[] = {0, 3, 4, 7, 8, 11, 12, 15, 0x18, 0x1b, 0x1c, 0x1e, 0x20, 0x21,
        0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2c, 0x2d, 0x2e};
    for (uint32_t op : ops) for (uint32_t mask = 0; mask < 16; ++mask) for (uint32_t seed = 1; seed <= 8; ++seed) {
        p.reset(); p.seed(seed * 1500 + op * 16 + mask);
        p.instruction(0, 0, upper(op, mask)); p.instruction(8, 0, nop); p.instruction(16, 0, nop | ebit); p.instruction(24, 0, nop);
        p.run(false, 1); p.run(true, 32);
    }
    for (uint32_t op = 0; op <= 0x2e; ++op) for (uint32_t mask = 0; mask < 16; ++mask) {
        if (op >= 0x2b && op <= 0x2b) continue;
        p.reset(); p.seed(0x2345 + op * 16 + mask);
        p.instruction(0, 0, special(op, mask, 3, 2));
        p.instruction(8, 0, nop | ebit); p.instruction(16, 0, nop);
        p.run(false, 1); p.run(true, 32);
    }
    // Random full IEEE inputs exercise product-sum classification, cancellation,
    // normalization and separate float multiplication/addition rounding.
    std::mt19937 arithmeticBits(0x3900cafe);
    constexpr uint32_t sums[] = {8u,9u,10u,11u,12u,13u,14u,15u,0x21u,0x23u,0x25u,0x27u,0x29u,0x2du,0x2eu};
    for (uint32_t sample = 0; sample < 50000; ++sample) {
        p.reset(); p.seed(sample + 12345);
        auto &state = p.c->state();
        for (float &lane : state.vf[2]) lane = std::bit_cast<float>(arithmeticBits());
        for (float &lane : state.vf[3]) lane = std::bit_cast<float>(arithmeticBits());
        for (float &lane : state.acc) lane = std::bit_cast<float>(arithmeticBits());
        state.q = std::bit_cast<float>(arithmeticBits()); state.i = std::bit_cast<float>(arithmeticBits());
        std::memcpy(&p.r->state(), &state, sizeof(state));
        const uint32_t op = sums[arithmeticBits() % std::size(sums)];
        const uint32_t mask = arithmeticBits() & 15u;
        const uint32_t instr = (sample & 1u) ? upper(op, mask) : special(op, mask, 3, 2);
        p.instruction(0, 0, instr | ebit); p.instruction(8, 0, nop);
        p.run(false, 1); p.run(true, 64);
    }
    constexpr uint32_t singles[] = {0u,1u,2u,3u,4u,5u,6u,7u,0x18u,0x19u,0x1au,0x1bu,
        0x1cu,0x1eu,0x20u,0x22u,0x24u,0x26u,0x28u,0x2au,0x2cu,0x2eu};
    for (uint32_t sample = 0; sample < 10000; ++sample) {
        p.reset(); p.seed(sample + 67890);
        auto &state = p.c->state();
        for (float &lane : state.vf[2]) lane = std::bit_cast<float>(arithmeticBits());
        for (float &lane : state.vf[3]) lane = std::bit_cast<float>(arithmeticBits());
        state.q = std::bit_cast<float>(arithmeticBits()); state.i = std::bit_cast<float>(arithmeticBits());
        std::memcpy(&p.r->state(), &state, sizeof(state));
        const uint32_t op = singles[arithmeticBits() % std::size(singles)];
        const uint32_t mask = arithmeticBits() & 15u;
        const uint32_t instr = (sample & 1u) ? upper(op, mask) : special(op, mask, 3, 2);
        p.instruction(0, 0, instr | ebit); p.instruction(8, 0, nop);
        p.run(false, 1); p.run(true, 64);
    }
    constexpr uint32_t targeted[][3] = {
        {0x3f800001u,0x3f7ffffeu,0xbf800000u}, // separate-product cancellation
        {0x0d800001u,0x3f7ffffeu,0x8d800000u}, // normal float hides exact underflow
        {0x5fc00000u,0x5f2aaaa9u,0x73ffffffu}, // non-max normal float hides overflow
        {0x00800000u,0x3f800000u,0x00800000u}, // FLT_MIN product, result binade 2
        {0x80800000u,0x3f800000u,0x80800000u}, // negative result binade 2
        {0x7e000000u,0x3f800000u,0x7e000000u}, // result binade 253
        {0xfe000000u,0x3f800000u,0xfe000000u}, // negative result binade 253
        {0xbf800001u,0x3f7ffffeu,0x3f800000u},
        {0x7f7fffffu,0x3f800000u,0x65000000u}, // FLT_MAX + one double ULP
        {0x7f7fffffu,0x3f800000u,0x64800000u},
        {0xff7fffffu,0x3f800000u,0xe5000000u},
        {0x00800000u,0x3f000000u,0u}, // subnormal exact sums
        {0x80800000u,0x3f000000u,0x80000000u},
        {0x00800000u,0x3f800001u,0x80800000u},
        {0x80000000u,0x3f800000u,0u},
        {0u,0xbf800000u,0x80000000u},
    };
    for (const auto &values : targeted) for (uint32_t op : sums) for (uint32_t mask = 0; mask < 16; ++mask) {
        for (bool accumulator : {false,true}) {
            p.reset(); p.seed(5682);
            for (float &lane : p.c->state().vf[2]) lane = std::bit_cast<float>(values[0]);
            for (float &lane : p.c->state().vf[3]) lane = std::bit_cast<float>(values[1]);
            for (float &lane : p.c->state().acc) lane = std::bit_cast<float>(values[2]);
            p.c->state().q = p.c->state().i = std::bit_cast<float>(values[1]);
            std::memcpy(&p.r->state(), &p.c->state(), sizeof(VU1State));
            const uint32_t instr = accumulator ? special(op, mask, 3, 2) : upper(op, mask);
            p.instruction(0, 0, instr | ebit); p.instruction(8, 0, nop);
            p.run(false, 1); p.run(true, 64);
        }
    }
    // Margin boundaries: result/product binade gaps around 21, first/last
    // normal binades, every destination mask and both destination kinds.
    for (uint32_t exponent : {1u,2u,3u,21u,22u,23u,24u,25u,26u,27u,125u,126u,127u,128u,231u,232u,252u,253u,254u}) {
        for (uint32_t gap : {20u,21u,22u,23u}) for (uint32_t mask = 0; mask < 16; ++mask) {
            for (bool subtract : {false,true}) for (bool accumulator : {false,true}) {
                p.reset(); p.seed(exponent * 4000 + gap * 100 + mask);
                for (float &lane : p.c->state().vf[2]) lane = std::bit_cast<float>((exponent << 23u) | 1u);
                for (float &lane : p.c->state().vf[3]) lane = std::bit_cast<float>(0x3f800000u - (1u << (24u-gap)));
                for (float &lane : p.c->state().acc) lane = std::bit_cast<float>((exponent << 23u) | (subtract ? 0u : 0x80000000u));
                p.c->state().q = p.c->state().i = p.c->state().vf[3][0];
                std::memcpy(&p.r->state(), &p.c->state(), sizeof(VU1State));
                const uint32_t op = subtract ? 0x2du : 0x29u;
                p.instruction(0, 0, (accumulator ? special(op,mask,3,2) : upper(op,mask)) | ebit);
                p.instruction(8, 0, nop); p.run(false,1); p.run(true,64);
            }
        }
    }
    // Long-to-short, back-to-back, wrapped and multiple GIFtag transfers.
    for (uint32_t format = 0; format <= 2; ++format) for (uint32_t loops : {0u, 1u, 3u, 31u, 255u}) {
        p.reset(); p.seed(loops + format * 256);
        p.packet(0, loops, format, 1, 0x53);
        p.packet(0x3000, 1, 2, 0, 0xa7);
        p.c->state().vi[1] = p.r->state().vi[1] = 0;
        p.c->state().vi[2] = p.r->state().vi[2] = 0x300;
        p.instruction(0, lowerSpecial(0x6c, 1), nop);
        p.instruction(8, lowerSpecial(0x6c, 2), nop | ebit); p.instruction(16, 0, nop);
        p.run(false, 1); p.run(true, 65536);
        p.packet(0, 0, 2, 0, 0x11); p.run(false, 65536);
    }
    for (uint32_t offset : {0x3ff0u, 0x3fe0u}) {
        p.reset(); p.packet(offset, 3, 2, 0, 0x6b);
        p.c->state().vi[1] = p.r->state().vi[1] = offset / 16;
        p.instruction(0, lowerSpecial(0x6c, 1), nop | ebit); p.instruction(8, 0, nop);
        p.run();
    }
    p.reset(); p.packet(0, 3, 0, 2, 0x21, false); p.packet(112, 3, 1, 2, 0x32, false); p.packet(176, 1, 2, 0, 0x43);
    p.instruction(0, lowerSpecial(0x6c, 0), nop | ebit); p.instruction(8, 0, nop); p.run(false, 1); p.run(true);
    // Store commit before PATH1 consumes a payload, and branch delay slots.
    p.reset(); p.packet(0, 2, 2, 0, 0x33); p.seed(66);
    p.c->state().vi[1] = p.r->state().vi[1] = 1;
    p.instruction(0, lowerSpecial(0x6c, 0), nop);
    p.instruction(8, (1u << 25) | (15u << 21) | (1u << 16) | (2u << 11), nop);
    p.instruction(16, (0x20u << 25) | 1, nop); p.instruction(24, 0, nop); p.instruction(32, 0, nop | ebit); p.instruction(40, 0, nop);
    for (uint32_t i = 0; i < 12; ++i) p.run(i != 0, 1);
    // Dependency stalls at every budget boundary, including Q/P resource
    // deadlines, VF/VI/ACC forwarding, and one-cycle resumptions.
    for (uint32_t budget = 1; budget <= 64; ++budget) {
        p.reset(); p.seed(5678);
        for (auto &vf : p.c->state().vf) for (float &lane : vf) lane = 1.0f;
        for (float &lane : p.c->state().acc) lane = 1.0f;
        std::memcpy(&p.r->state(), &p.c->state(), sizeof(VU1State));
        p.instruction(0, 0, upper(0x28, 15, 3, 2, 4));
        p.instruction(8, lowerSpecial(0x38, 4, 3), upper(0x29, 15, 3, 4, 5));
        p.instruction(16, lowerSpecial(0x38, 4, 3), special(0x29, 15, 3, 5));
        p.instruction(24, lowerSpecial(0x3b, 0), upper(0x29, 15, 3, 5, 6));
        p.instruction(32, lowerSpecial(0x70, 4), nop);
        p.instruction(40, lowerSpecial(0x78, 4), nop);
        p.instruction(48, lowerSpecial(0x7b, 0), nop);
        p.instruction(56, lowerSpecial(0x3c, 4, 1), nop);
        p.instruction(64, 0x80000030u | (2u << 6) | (1u << 11) | (1u << 16), nop);
        p.instruction(72, (0x28u << 25) | (2u << 16) | (1u << 11) | 1u, nop);
        p.instruction(80, 0, nop | ebit); p.instruction(88, 0, nop | ebit); p.instruction(96, 0, nop);
        p.run(false, budget);
        for (uint32_t cycle = 0; cycle < 100; ++cycle) p.run(true, 1);
    }
    for (uint32_t budget = 1; budget <= 12; ++budget) {
        p.reset(); p.seed(5679); p.packet(0, 31, 2, 0, 0x7d);
        p.c->state().vf[2][0] = p.r->state().vf[2][0] = 1.0f;
        p.c->state().vi[1] = p.r->state().vi[1] = 0;
        p.instruction(0, lowerSpecial(0x6c, 1), nop);
        p.instruction(8, lowerSpecial(0x3c, 2, 1), nop);
        p.instruction(16, lowerSpecial(0x6c, 1), nop | ebit); p.instruction(24, 0, nop);
        p.run(false, budget);
        for (uint32_t cycle = 0; cycle < 160; ++cycle) p.run(true, 1);
    }
    // Same generation-tracked code storage with isolated pair changes, including
    // repeated writes of identical bytes and I-bit changes to lower-word meaning.
    p.reset(); p.seed(5680);
    p.instruction(0, 0, upper(0x28) | ebit); p.instruction(8, 0, nop); p.run();
    for (uint32_t op : ops) {
        p.instruction(0, 0, upper(op) | ebit); p.run();
        p.instruction(0, 0, upper(op) | ebit); p.run();
        p.instruction(0, 0x3f800000u, upper(op) | ebit | 0x80000000u); p.run();
    }
    // Abort an incomplete packet with a new execute and then emit a shorter packet.
    p.reset(); p.packet(0, 255, 2, 0, 0x5a); p.instruction(0, lowerSpecial(0x6c, 0), nop);
    p.instruction(8, 0, nop | ebit); p.instruction(16, 0, nop); p.run(false, 1);
    p.packet(0, 1, 2, 0, 0xc5); p.run();
}

#include "pipeline_cases.inc"

template<class T> void read(std::ifstream &input, T &value) {
    if (!input.read(reinterpret_cast<char *>(&value), sizeof(value))) throw std::runtime_error("truncated VU recording");
}
void bytes(std::ifstream &input, void *dest, size_t length) {
    if (!input.read(static_cast<char *>(dest), static_cast<std::streamsize>(length))) throw std::runtime_error("truncated VU recording payload");
}
void replay(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    std::array<char,8> magic{}; bytes(input, magic.data(), magic.size());
    uint32_t version, stateSize; read(input, version); read(input, stateSize);
    if (magic != std::array<char,8>{'K','F','I','V','V','U','3','9'} || version != 1 || stateSize != sizeof(VU1State)) throw std::runtime_error("recording header mismatch");
    Pair p; bool first = true; uint64_t records = 0;
    while (input.peek() != EOF) {
        std::array<uint32_t,9> h{}; bytes(input, h.data(), sizeof(h));
        const bool resume = h[0] != 0;
        if (h[5] != PS2_VU1_CODE_SIZE || h[6] != PS2_VU1_DATA_SIZE || h[7] > 4096 || h[8] != 0) throw std::runtime_error("recording dimensions mismatch");
        VU1State before{}, after{}; read(input, before); read(input, after);
        std::array<uint8_t,PS2_VU1_CODE_SIZE> code{}; std::array<uint8_t,PS2_VU1_DATA_SIZE> dataBefore{}, dataAfter{};
        bytes(input, code.data(), code.size()); bytes(input, dataBefore.data(), dataBefore.size()); bytes(input, dataAfter.data(), dataAfter.size());
        Packets expected;
        for (uint32_t packet = 0; packet < h[7]; ++packet) {
            uint32_t length; read(input, length); if (length > 65536) throw std::runtime_error("oversized captured packet");
            expected.emplace_back(length); bytes(input, expected.back().data(), length);
        }
        if (first) {
            if (resume) throw std::runtime_error("capture must start on execute boundary");
            p.c->restoreForReplay(before); VU1StateRef refBefore{}; std::memcpy(&refBefore, &before, sizeof(before)); p.r->restoreForReplay(refBefore);
            first = false;
        } else {
            // Guest code can update D/T enables between callbacks. Other VU state must match contiguous execution.
            p.c->state().dBitEnabled = before.dBitEnabled; p.c->state().tBitEnabled = before.tBitEnabled;
            p.r->state().dBitEnabled = before.dBitEnabled; p.r->state().tBitEnabled = before.tBitEnabled;
            p.check(std::memcmp(&p.c->state(), &before, sizeof(before)) == 0, "recorded-before-state");
        }
        for (uint32_t at = 0; at < code.size(); at += 8) {
            uint64_t value; std::memcpy(&value, code.data() + at, 8);
            uint64_t previous; std::memcpy(&previous, p.cm->getVU1Code() + at, 8);
            if (first || previous != value) {
                p.cm->write64(PS2_VU1_CODE_BASE + at, value); p.rm->write64(PS2_VU1_CODE_BASE + at, value);
            }
        }
        p.data(0, dataBefore.data(), uint32_t(dataBefore.size()));
        p.run(resume, h[4], h[1], h[2], h[3]);
        p.check(std::memcmp(&p.c->state(), &after, sizeof(after)) == 0, "recorded-after-state");
        p.check(std::memcmp(p.cm->getVU1Data(), dataAfter.data(), dataAfter.size()) == 0, "recorded-after-memory");
        p.check(p.cp == expected, "recorded-packets"); ++records;
    }
    if (records == 0) throw std::runtime_error("empty VU recording");
    std::cout << "Retail contiguous replay records=" << records << " candidate-ns=" << candidateNanoseconds
        << " reference-ns=" << referenceNanoseconds << " speedup=" << double(referenceNanoseconds) / double(candidateNanoseconds) << '\n';
}
}
#include "metadata_cases.inc"

int main(int argc, char **argv) {
    try {
        static_assert(sizeof(VU1State) == sizeof(VU1StateRef));
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg.starts_with("--control=")) control = arg.substr(10);
            else if (arg == "--metadata") { metadataExact(false); return 0; }
            else if (arg == "--metadata-control") { metadataExact(true); return 0; }
            else if (arg.starts_with("--replay=")) { replay(arg.substr(9)); std::cout << "PASS checks=" << checks << " calls=" << calls << '\n'; return 0; }
            else throw std::runtime_error("unknown argument");
        }
        synthetic();
        pipelineSynthetic();
        if (!control.empty()) throw std::runtime_error("negative control was not detected");
        std::cout << "PASS current39 exact VU checks=" << checks << " calls=" << calls << '\n'; return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
