// SPDX-License-Identifier: GPL-3.0-only
// Synthetic comparison of identical actual SPU source under compiler inlining controls.
#include "iop_spu2.h"
#include "iop_spu2_ref.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ps2x::iop::detail;
namespace {
enum class Category { Pcm, Chunks, Irq, Statistics, Mmio, Ram, Count };
std::array<uint64_t, size_t(Category::Count)> mismatches{};
uint64_t checks = 0, samplesCompared = 0, ramBytesCompared = 0;
bool negativePcm = false;
void check(bool condition, Category category, const char *label) {
    ++checks;
    if (!condition) {
        ++mismatches[size_t(category)];
        if (!negativePcm) throw std::runtime_error(label);
    }
}
uint32_t random(uint32_t &seed) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed;
}
template<class T> void addr(T &spu, unsigned core, unsigned offset, uint32_t value) {
    spu.write16(core * 0x400u + offset, uint16_t(value >> 16));
    spu.write16(core * 0x400u + offset + 2u, uint16_t(value));
}
template<class T> void upload(T &spu, unsigned core, uint32_t word, std::vector<uint8_t> data) {
    addr(spu, core, 0x1a8u, word);
    spu.dmaTransfer(core, true, data.data(), uint32_t(data.size() / 4u));
    spu.completeDma(core);
}
template<class T> void setup(T &spu, unsigned scenario) {
    uint32_t seed = 0x623ffe81u ^ (scenario * 0x9e3779b9u);
    std::vector<uint8_t> ram(2u << 20u);
    for (auto &v : ram) v = uint8_t(random(seed));
    // Every directed voice has a valid recurring block, so the long-run
    // workload continues exercising Gaussian/ADPCM/envelope work.
    for (unsigned index = 0; index != 24; ++index) {
        const size_t byte = size_t(0x8000u + index * 0x100u) * 2u;
        ram[byte] = uint8_t((index % 5u) * 16u + scenario % 13u);
        ram[byte + 1u] = 7u;
    }
    upload(spu, 0u, 0u, std::move(ram));
    for (unsigned core = 0; core != 2; ++core) {
        const unsigned bank = core * 0x400u, ext = 0x760u + core * 0x28u;
        spu.write16(bank + 0x19au, uint16_t(0xc0c0u | (scenario & 15u) << 10u));
        spu.write16(bank + 0x198u, 0xfffu);
        for (unsigned offset : {0x180u, 0x184u, 0x188u, 0x18cu, 0x190u, 0x194u}) {
            const uint32_t mask = offset == 0x184u && scenario % 3u ? 0u : random(seed) & 0xffffffu;
            spu.write16(bank + offset, uint16_t(mask));
            spu.write16(bank + offset + 2u, uint16_t(mask >> 16u));
        }
        for (unsigned i = 0; i != 20; ++i) spu.write16(ext + i * 2u, uint16_t(random(seed)));
        spu.write16(ext, 0x3fffu); spu.write16(ext + 2u, 0x3fffu);
        const uint32_t start = scenario % 3u == 0u ? 0x80000u : 0x81231u;
        addr(spu, core, 0x2e0u, start); spu.write16(bank + 0x33cu, 8u);
        for (unsigned offset = 0x2e4u; offset < 0x33cu; offset += 4u)
            addr(spu, core, offset, random(seed));
        // IRQs exercise per-voice and per-core capture writes and RAM reads.
        addr(spu, core, 0x19cu, scenario % 2u ? 0x400u + core * 0x800u : 0x1000u + core * 0x800u);
        const unsigned voices = std::array<unsigned, 4>{0u, 1u, 12u, 24u}[scenario % 4u];
        for (unsigned index = 0; index != 24; ++index) {
            const unsigned voice = bank + index * 16u;
            spu.write16(voice, uint16_t(random(seed)));
            spu.write16(voice + 2u, uint16_t(random(seed)));
            spu.write16(voice + 4u, uint16_t((scenario & 1u) ? 0x1000u : random(seed) & 0x3fffu));
            spu.write16(voice + 6u, uint16_t(scenario % 3u ? 0x000fu : random(seed)));
            spu.write16(voice + 8u, uint16_t(random(seed)));
            addr(spu, core, 0x1c0u + index * 12u, 0x8000u + index * 0x100u);
            addr(spu, core, 0x1c4u + index * 12u, 0x8000u + index * 0x100u);
        }
        const uint32_t keys = voices ? (1u << voices) - 1u : 0u;
        spu.write16(bank + 0x1a0u, uint16_t(keys));
        spu.write16(bank + 0x1a2u, uint16_t(keys >> 16u));
    }
}
struct Output {
    std::vector<int16_t> pcm;
    std::vector<size_t> chunkSizes;
    std::vector<std::array<uint64_t, 3>> irq;
    bool injectPcm = false, injected = false;
};
template<class T> void callbacks(T &spu, Output &output) {
    spu.setPcmCallback([&output](std::span<const int16_t> chunk) {
        const size_t first = output.pcm.size();
        output.pcm.insert(output.pcm.end(), chunk.begin(), chunk.end());
        output.chunkSizes.push_back(chunk.size());
        // Controlled observation corruption, not substituted hardware. The
        // actual candidate executes all the same writes and IRQs. The control
        // must identify only PCM mismatch while other categories stay exact.
        if (output.injectPcm && !output.injected && !chunk.empty()) {
            output.pcm[first] ^= 1;
            output.injected = true;
        }
    });
    spu.setIrqCallback([&spu, &output] {
        output.irq.push_back({spu.statistics().frames, spu.read16(0x7c2u), spu.statistics().irqAssertions});
    });
}
template<class T> std::array<uint64_t, 8> stats(T &spu) {
    const auto s = spu.statistics();
    return {s.frames, s.nonzeroFrames, s.dmaWrites, s.dmaReads, s.dmaBytes,
            s.keyOns, s.decodedBlocks, s.irqAssertions};
}
template<class T> std::vector<uint8_t> ram(T &spu) {
    addr(spu, 0u, 0x1a8u, 0u);
    std::vector<uint8_t> result(2u << 20u);
    spu.dmaTransfer(0u, false, result.data(), uint32_t(result.size() / 4u));
    return result;
}
template<class A, class B> void compare(A &a, B &b, Output &oa, Output &ob, bool fullRam) {
    a.flushPcm(); b.flushPcm();
    check(oa.pcm == ob.pcm, Category::Pcm, "PCM samples differ");
    check(oa.chunkSizes == ob.chunkSizes, Category::Chunks, "PCM chunk boundaries differ");
    check(oa.irq == ob.irq, Category::Irq, "IRQ order/frame/register state differs");
    check(stats(a) == stats(b), Category::Statistics, "SPU statistics differ");
    for (uint32_t offset = 0; offset != 0x800u; offset += 2u) {
        // PIO DATA reads have side effects; all other visible MMIO is compared.
        if (offset == 0x1acu || offset == 0x5acu) continue;
        check(a.read16(offset) == b.read16(offset), Category::Mmio, "MMIO register differs");
    }
    samplesCompared += oa.pcm.size(); oa.pcm.clear(); ob.pcm.clear();
    oa.chunkSizes.clear(); ob.chunkSizes.clear(); oa.irq.clear(); ob.irq.clear();
    if (fullRam) {
        check(ram(a) == ram(b), Category::Ram, "Full 2 MiB SPU RAM differs");
        ramBytesCompared += 2u << 20u;
        check(stats(a) == stats(b), Category::Statistics, "RAM snapshot side effects differ");
    }
}
template<class T> void command(T &spu, unsigned op, uint32_t value, uint32_t extra) {
    const unsigned core = (value >> 30u) & 1u, bank = core * 0x400u;
    switch (op % 12u) {
    case 0: spu.advance(uint64_t(value % 64u + 1u) * 768u + (extra % 768u)); break;
    case 1: spu.write16(bank + ((value % 24u) * 16u) + 4u, uint16_t(extra)); break;
    case 2: spu.write16(bank + ((value % 24u) * 16u), uint16_t(extra)); break;
    case 3: spu.write16(bank + 0x1a0u + ((value >> 2u) & 2u), uint16_t(extra)); break;
    case 4: spu.write16(bank + 0x1a4u + ((value >> 2u) & 2u), uint16_t(extra)); break;
    case 5: spu.write16(bank + 0x180u + (value % 12u) * 2u, uint16_t(extra)); break;
    case 6: addr(spu, core, 0x1c4u + (value % 24u) * 12u, extra & 0xfffffu); break;
    case 7: {
        std::vector<uint8_t> data(128u);
        uint32_t seed = extra;
        for (auto &v : data) v = uint8_t(random(seed));
        upload(spu, core, value & 0xfffffu, std::move(data)); break;
    }
    case 8: spu.write16(bank + 0x19au, uint16_t(0xc080u | extra & 0x3f40u)); break;
    case 9: spu.write16(0x760u + core * 0x28u + (value % 20u) * 2u, uint16_t(extra)); break;
    case 10: {
        spu.write16(bank + 0x1b0u, uint16_t(1u << core));
        std::vector<uint8_t> data(1024u);
        uint32_t seed = extra;
        for (auto &v : data) v = uint8_t(random(seed));
        spu.dmaTransfer(core, true, data.data(), uint32_t(data.size() / 4u));
        spu.completeDma(core); break;
    }
    case 11: spu.cancelDma(core); spu.write16(bank + 0x1b0u, 0u); break;
    }
}
template<class T> int benchmark(unsigned scenario) {
    T spu; setup(spu, scenario);
    uint64_t hash = 1469598103934665603ull, chunks = 0;
    spu.setPcmCallback([&](auto pcm) {
        ++chunks; for (const int16_t sample : pcm) { hash ^= uint16_t(sample); hash *= 1099511628211ull; }
    });
    const auto begin = std::chrono::steady_clock::now();
    spu.advance(480000ull * 768u); spu.flushPcm();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    const auto s = spu.statistics();
    std::printf("scenario=%u seconds=%.9f frames=%" PRIu64 " nonzero=%" PRIu64
                " blocks=%" PRIu64 " chunks=%" PRIu64 " hash=%" PRIu64 "\n",
                scenario, seconds, s.frames, s.nonzeroFrames, s.decodedBlocks, chunks, hash);
    return 0;
}
}
int main(int argc, char **argv) try {
    if (argc > 2 && std::string(argv[1]) == "--benchmark") {
        if (argc > 4) throw std::runtime_error("too many benchmark arguments");
        const unsigned scenario = argc > 3 ? unsigned(std::stoul(argv[3])) : 3u;
        if (scenario >= 24u) throw std::runtime_error("benchmark scenario must be 0..23");
        const std::string mode = argv[2];
        if (mode == "baseline") return benchmark<IopSpu2Ref>(scenario);
        if (mode == "candidate") return benchmark<IopSpu2>(scenario);
        throw std::runtime_error("benchmark mode must be baseline or candidate");
    }
    if (argc == 2 && std::string(argv[1]) == "--negative-pcm") negativePcm = true;
    else if (argc != 1) throw std::runtime_error("usage: spu_inlining_tests [--negative-pcm | --benchmark baseline|candidate [scenario]]");
    for (unsigned scenario = 0; scenario != 24; ++scenario) {
        IopSpu2Ref baseline; IopSpu2 candidate;
        Output oldOutput, newOutput;
        newOutput.injectPcm = negativePcm && scenario == 0u;
        callbacks(baseline, oldOutput); callbacks(candidate, newOutput);
        setup(baseline, scenario); setup(candidate, scenario);
        uint32_t seed = 0xc0a47611u ^ (scenario * 0x9e3779b9u);
        baseline.advance(4096ull * 768u + 513u); candidate.advance(4096ull * 768u + 513u);
        compare(baseline, candidate, oldOutput, newOutput, false);
        for (unsigned operation = 0; operation != 192; ++operation) {
            const uint32_t value = random(seed), extra = random(seed);
            command(baseline, operation, value, extra); command(candidate, operation, value, extra);
            baseline.advance(uint64_t(value % 128u + 1u) * 768u + extra % 768u);
            candidate.advance(uint64_t(value % 128u + 1u) * 768u + extra % 768u);
            if (operation % 32u == 31u) compare(baseline, candidate, oldOutput, newOutput, false);
        }
        baseline.advance(2000ull * 768u); candidate.advance(2000ull * 768u);
        compare(baseline, candidate, oldOutput, newOutput, true);
        baseline.reset(); candidate.reset(); baseline.advance(256u * 768u); candidate.advance(256u * 768u);
        compare(baseline, candidate, oldOutput, newOutput, false);
    }
    if (negativePcm) {
        const bool onlyPcm = mismatches[size_t(Category::Pcm)] == 1u &&
            std::all_of(mismatches.begin() + 1, mismatches.end(), [](uint64_t count) { return count == 0u; });
        if (!onlyPcm || samplesCompared == 0u || ramBytesCompared == 0u)
            throw std::runtime_error("negative control did not isolate exactly one PCM mismatch");
        std::puts("PASS expected PCM-only corruption detected; chunks/IRQ/statistics/MMIO/RAM exact");
    }
    std::printf("PASS scenarios=24 checks=%" PRIu64 " PCM samples=%" PRIu64 " full RAM bytes=%" PRIu64 "\n",
                checks, samplesCompared, ramBytesCompared);
    return 0;
} catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
