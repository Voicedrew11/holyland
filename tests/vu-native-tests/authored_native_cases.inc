// SPDX-License-Identifier: GPL-3.0-or-later
// Source-authored exact compiled-catalog cases. Include outside the verifier's
// unnamed namespace; Pair and its encoding helpers remain visible in this TU.
#ifdef VU_SPECIALIZATION_VERIFY_COUNTER
uint64_t vuNativeIssuedForTests();
#endif
#include "authored_catalog.inc"

namespace {
uint64_t authoredNativeIssued()
{
#ifdef VU_SPECIALIZATION_VERIFY_COUNTER
    return vuNativeIssuedForTests();
#else
    throw std::runtime_error("Authored native proof requires the issue counter");
#endif
}

void authoredLoad(Pair &p)
{
    static_assert(std::endian::native == std::endian::little);
    static_assert(sizeof(authoredVU::pairs) == PS2_VU1_CODE_SIZE);
    std::memcpy(p.cm->getVU1Code(), authoredVU::pairs, sizeof(authoredVU::pairs));
    std::memcpy(p.rm->getVU1Code(), authoredVU::pairs, sizeof(authoredVU::pairs));
    // Publish the complete raw upload through the normal tracked write API.
    p.instruction(0, authoredVU::pairs[0][0], authoredVU::pairs[0][1]);
    p.c->state().vi[1] = p.r->state().vi[1] = 0;
    p.c->state().vi[2] = p.r->state().vi[2] = 1;
    p.packet(0, 31, 2, 0, 0x5a);
}

void authoredRawPair(Pair &p, uint32_t pc, uint32_t lo, uint32_t hi)
{
    const uint64_t value = lo | (uint64_t(hi) << 32u);
    std::memcpy(p.cm->getVU1Code() + pc, &value, sizeof(value));
    std::memcpy(p.rm->getVU1Code() + pc, &value, sizeof(value));
}

void authoredCustomRun(Pair &p, uint8_t *ccode, uint8_t *rcode,
                       uint32_t codeSize, PS2Memory *cmemory, PS2Memory *rmemory,
                       uint32_t start, uint32_t budget = 16,
                       uint8_t *cdata = nullptr, uint8_t *rdata = nullptr,
                       uint32_t dataSize = PS2_VU1_DATA_SIZE)
{
    p.cp.clear(); p.rp.clear(); ++calls;
    if (cdata == nullptr) cdata = p.cm->getVU1Data();
    if (rdata == nullptr) rdata = p.rm->getVU1Data();
    p.c->execute(ccode, codeSize, cdata, dataSize, p.gs, cmemory, start, 0, 0, budget);
    p.r->execute(rcode, codeSize, rdata, dataSize, p.gs, rmemory, start, 0, 0, budget);
    p.compare();
    p.check(std::memcmp(cdata, rdata, dataSize) == 0, "authored-custom-data");
}

void authoredOriginalNativeSynthetic()
{
    Pair p;
    const uint64_t initialIssues = authoredNativeIssued();
    // Each program starts near or crosses a group edge; seed full IEEE float
    // encodings, aliases and public high STATUS bits and preserve pending
    // pipelines across short resumes. D/T masks exercise branch halt delay.
    for (uint32_t entry : {authoredVU::arithmetic, authoredVU::pipelines,
                          authoredVU::branches, authoredVU::stores,
                          authoredVU::packets, authoredVU::wrap})
    {
        for (uint32_t salt = 0; salt < 8; ++salt)
        {
            for (uint32_t budget : {1u,2u,3u,4u,7u,16u,17u,33u,64u})
            {
                p.reset(); p.seed(entry ^ (salt * 0x912341u) ^ budget);
                p.c->state().status = (salt & 1u) ? 0xdeadffffu : 0x80000fc0u;
                p.c->state().dBitEnabled = (salt & 2u) != 0;
                p.c->state().tBitEnabled = (salt & 4u) != 0;
                std::memcpy(&p.r->state(), &p.c->state(), sizeof(VU1State));
                authoredLoad(p);
                if (entry == authoredVU::packets)
                    p.c->state().vf[2][0] = p.r->state().vf[2][0] = 1.0f;
                const uint64_t before = authoredNativeIssued();
                p.run(false, budget, entry);
                p.check(authoredNativeIssued() > before, "authored-native-entry-exercised");
                for (uint32_t resume = 0; resume < 32; ++resume) p.run(true, 1);
                p.run(true, 64);
            }
        }
    }
    // Every group edge, adjacent entry and full-memory wrap use source-authored
    // instructions; no arbitrary retail program coverage is implied.
    for (uint32_t edge = 0; edge < PS2_VU1_CODE_SIZE; edge += 128u)
    {
        for (uint32_t delta : {16368u,16376u,0u,8u})
        {
            p.reset(); p.seed(edge ^ delta); authoredLoad(p);
            const uint64_t edgeBefore = authoredNativeIssued();
            p.run(false, 17, (edge + delta) & 0x3fffu); p.run(true, 3);
            p.check(authoredNativeIssued() > edgeBefore, "authored-native-group-edge-exercised");
        }
    }
    p.check(authoredNativeIssued() > initialIssues, "authored-native-total-exercised");

    // Admission must describe the actual cache, including intentionally stale
    // entries after an unmarked raw write. Both directions are tested, then a
    // tracked generation change restores catalog eligibility.
    p.reset(); p.seed(0x35451); authoredLoad(p);
    p.run(false, 1, authoredVU::arithmetic);
    const uint32_t at = authoredVU::arithmetic;
    authoredRawPair(p, at, 0, upper(0x2a, 15, 3, 2, 4));
    uint64_t before = authoredNativeIssued();
    p.run(false, 8, at);
    p.check(authoredNativeIssued() > before, "authored-matched-unmarked-change");
    p.instruction(at, 0, upper(0x2a, 15, 3, 2, 4));
    before = authoredNativeIssued(); p.run(false, 8, at);
    p.check(authoredNativeIssued() == before, "authored-tracked-change-fallback");
    p.instruction(at, authoredVU::pairs[at / 8][0], authoredVU::pairs[at / 8][1]);
    before = authoredNativeIssued(); p.run(false, 8, at);
    p.check(authoredNativeIssued() > before, "authored-tracked-restore-admission");

    p.reset(); p.seed(0x35452); authoredLoad(p);
    const uint32_t unused = authoredVU::unused;
    p.instruction(unused, 0, nop | ebit);
    before = authoredNativeIssued(); p.run(false, 1, at);
    p.check(authoredNativeIssued() == before, "authored-unexecuted-pair-rejects-catalog");
    authoredRawPair(p, unused, authoredVU::pairs[unused / 8][0], authoredVU::pairs[unused / 8][1]);
    before = authoredNativeIssued(); p.run(false, 8, at);
    p.check(authoredNativeIssued() == before, "authored-unmatched-unmarked-restore");
    p.instruction(unused, authoredVU::pairs[unused / 8][0], authoredVU::pairs[unused / 8][1]);
    before = authoredNativeIssued(); p.run(false, 8, at);
    p.check(authoredNativeIssued() > before, "authored-unmatched-tracked-restore");

    // Preserve cache binding when the same interpreter receives a different
    // memory owner. Keep old owners alive so allocator address reuse cannot
    // disguise the key transition.
    auto oldCandidate = std::move(p.cm), oldReference = std::move(p.rm);
    p.cm = std::make_unique<PS2Memory>(); p.rm = std::make_unique<PS2Memory>();
    if (!p.cm->initialize() || !p.rm->initialize()) throw std::runtime_error("authored-owner-init");
    p.cm->setGifPacketCallback([&p](const uint8_t *data, uint32_t bytes) { p.cp.emplace_back(data, data + bytes); });
    p.rm->setGifPacketCallback([&p](const uint8_t *data, uint32_t bytes) { p.rp.emplace_back(data, data + bytes); });
    authoredLoad(p); before = authoredNativeIssued(); p.run(false, 8, at);
    p.check(authoredNativeIssued() > before, "authored-new-owner-admission");

    // Untracked buffers and null memory must bypass an earlier matched cache.
    std::array<uint8_t, PS2_VU1_CODE_SIZE> cc{}, rc{};
    std::memcpy(cc.data(), authoredVU::pairs, sizeof(authoredVU::pairs)); rc = cc;
    before = authoredNativeIssued();
    authoredCustomRun(p, cc.data(), rc.data(), uint32_t(cc.size()), p.cm.get(), p.rm.get(), at);
    p.check(authoredNativeIssued() == before, "authored-untracked-buffer-fallback");
    before = authoredNativeIssued();
    authoredCustomRun(p, p.cm->getVU1Code(), p.rm->getVU1Code(), PS2_VU1_CODE_SIZE, nullptr, nullptr, at);
    p.check(authoredNativeIssued() == before, "authored-null-memory-fallback");
    before = authoredNativeIssued();
    authoredCustomRun(p, p.cm->getVU1Code(), p.rm->getVU1Code(), PS2_VU1_CODE_SIZE - 8u, p.cm.get(), p.rm.get(), at);
    p.check(authoredNativeIssued() == before, "authored-short-size-fallback");
    before = authoredNativeIssued(); p.run(false, 1, at);
    p.check(authoredNativeIssued() > before, "authored-full-size-readmission");
    before = authoredNativeIssued(); p.run(false, 1, at + 2u);
    p.check(authoredNativeIssued() == before, "authored-misaligned-pc-fallback");
    p.instruction(at, 0xfe000000u, nop);
    before = authoredNativeIssued(); p.run(false, 1, at);
    p.check(authoredNativeIssued() == before, "authored-reserved-op-fallback");

    Pair vu0;
    vu0.c = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU0);
    vu0.r = std::make_unique<VU1InterpreterRef>(VU1InterpreterRef::Unit::VU0);
    vu0.seed(0x3461);
    std::memcpy(vu0.cm->getVU0Code(), authoredVU::pairs, PS2_VU0_CODE_SIZE);
    std::memcpy(vu0.rm->getVU0Code(), authoredVU::pairs, PS2_VU0_CODE_SIZE);
    vu0.cm->write64(PS2_VU0_CODE_BASE, uint64_t(nop | ebit) << 32u);
    vu0.rm->write64(PS2_VU0_CODE_BASE, uint64_t(nop | ebit) << 32u);
    before = authoredNativeIssued();
    authoredCustomRun(vu0, vu0.cm->getVU0Code(), vu0.rm->getVU0Code(), PS2_VU0_CODE_SIZE,
                      vu0.cm.get(), vu0.rm.get(), at, 16,
                      vu0.cm->getVU0Data(), vu0.rm->getVU0Data(), PS2_VU0_DATA_SIZE);
    vu0.check(authoredNativeIssued() == before, "authored-vu0-fallback");

    // PATH1 callbacks alter the successor code, make it reserved, or alter
    // public PC while the next XGKICK stalls. The currently decoded pair must
    // still issue exactly as the reference; the next decode boundary observes
    // the mutation. Candidate/reference callbacks mutate their own memory.
    for (uint32_t kind = 0; kind < 3; ++kind)
    {
        Pair q; q.reset(); q.seed(0x562900u + kind); authoredLoad(q);
        q.c->state().vf[2][0] = q.r->state().vf[2][0] = 1.0f;
        bool candidateMutated = false, referenceMutated = false;
        auto callback = [&](bool candidate, const uint8_t *data, uint32_t bytes) {
            auto &packets = candidate ? q.cp : q.rp;
            packets.emplace_back(data, data + bytes);
            bool &mutated = candidate ? candidateMutated : referenceMutated;
            if (mutated) return;
            mutated = true;
            if (kind == 2) {
                if (candidate) ++q.c->state().pc; else ++q.r->state().pc;
            } else {
                const uint32_t next = authoredVU::packets + 24u;
                const uint64_t value = (uint64_t(nop | ebit) << 32u) | (kind ? 0xfe000000u : 0u);
                (candidate ? q.cm.get() : q.rm.get())->write64(PS2_VU1_CODE_BASE + next, value);
            }
        };
        q.cm->setGifPacketCallback([&](const uint8_t *data, uint32_t bytes) { callback(true, data, bytes); });
        q.rm->setGifPacketCallback([&](const uint8_t *data, uint32_t bytes) { callback(false, data, bytes); });
        const uint64_t issueStart = authoredNativeIssued();
        q.run(false, 96, authoredVU::packets); q.run(true, 1); q.run(true, 17);
        q.check(candidateMutated && referenceMutated, "authored-callback-exercised");
        q.check(authoredNativeIssued() > issueStart, "authored-callback-native-exercised");
    }
}
}

namespace {
void authoredNativeSynthetic()
{
    authoredOriginalNativeSynthetic();
    Pair p;
    std::mt19937 bits(0x601dcafeu);
    constexpr uint32_t exactCases[][3] = {
        {0x3f800001u,0x3f7ffffeu,0xbf800000u},
        {0x0d800001u,0x3f7ffffeu,0x8d800000u},
        {0x5fc00000u,0x5f2aaaa9u,0x73ffffffu},
        {0x00800000u,0x3f800000u,0x00800000u},
        {0x80800000u,0x3f800000u,0x80800000u},
        {0x7e000000u,0x3f800000u,0x7e000000u},
        {0xfe000000u,0x3f800000u,0xfe000000u},
        {0xbf800001u,0x3f7ffffeu,0x3f800000u},
        {0x7f7fffffu,0x3f800000u,0x65000000u},
        {0x7f7fffffu,0x3f800000u,0x64800000u},
        {0xff7fffffu,0x3f800000u,0xe5000000u},
        {0x00800000u,0x3f000000u,0u},
        {0x80800000u,0x3f000000u,0x80000000u},
        {0x00800000u,0x3f800001u,0x80800000u},
        {0x80000000u,0x3f800000u,0u},
        {0u,0xbf800000u,0x80000000u},
        {0x7fc12345u,0xff800000u,0x7f800000u},
    };
    for (uint32_t entry = 0; entry < authoredVU::allOpcodeCount; ++entry)
    {
        const uint32_t pc = authoredVU::allOpcodeBegin + entry * 8u;
        for (uint32_t sample = 0; sample < std::size(exactCases) + 8u; ++sample)
        {
            p.reset(); p.seed(0x346dd100u + entry * 71u + sample);
            auto &state = p.c->state();
            if (sample < std::size(exactCases))
            {
                for (float &lane : state.vf[2]) lane = std::bit_cast<float>(exactCases[sample][0]);
                for (float &lane : state.vf[3]) lane = std::bit_cast<float>(exactCases[sample][1]);
                for (float &lane : state.acc) lane = std::bit_cast<float>(exactCases[sample][2]);
                state.q = state.i = std::bit_cast<float>(exactCases[sample][1]);
            }
            else
            {
                for (auto &reg : state.vf) for (float &lane : reg) lane = std::bit_cast<float>(bits());
                for (float &lane : state.acc) lane = std::bit_cast<float>(bits());
                state.q = std::bit_cast<float>(bits()); state.i = std::bit_cast<float>(bits());
            }
            state.status = (sample & 1u) ? 0xdeadffffu : 0x80000fc0u;
            std::memcpy(&p.r->state(), &state, sizeof(VU1State));
            authoredLoad(p);
            const uint64_t before = authoredNativeIssued();
            p.run(false, 1, pc);
            p.check(authoredNativeIssued() > before, "all-upper-opcode-mask-native-issued");
            // Each intended instruction is issued alone, preserving its real
            // pending pipeline. Move both public PCs into the same authored
            // NOP/E drain and resume, then compare committed flags and timing.
            p.c->state().pc = p.r->state().pc = authoredVU::allOpcodeDrain;
            p.run(true, 32);
        }
    }
}
}
