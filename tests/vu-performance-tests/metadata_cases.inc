// Source-only descriptor equality/bounds cases; GPL-3.0 (see ../LICENSE).
// Both actual decoders run; this file contains no game-derived instructions.
void metadataExact(bool negativeControl)
{
    uint64_t pairs = 0;
    uint32_t maxReads = 0, maxWrites = 0;
    constexpr uint32_t masks[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u,
                                 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
    // Distinct registers reach maximal counts. Aliases exercise read-lane
    // merging, lower-write suppression and upper-to-lower shadow copies.
    constexpr uint32_t registers[][5] = {
        {2u, 3u, 4u, 6u, 7u}, {2u, 2u, 4u, 2u, 4u},
        {0u, 0u, 0u, 0u, 0u}, {31u, 16u, 15u, 31u, 15u},
        {2u, 3u, 4u, 4u, 2u},
    };
    for (bool vu0 : {false, true})
    {
        const auto unit = vu0 ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1;
        const auto refUnit = vu0 ? VU1InterpreterRef::Unit::VU0 : VU1InterpreterRef::Unit::VU1;
        auto candidate = std::make_unique<VU1Interpreter>(unit);
        auto reference = std::make_unique<VU1InterpreterRef>(refUnit);
        std::array<uint32_t, 128> observed{}, expected{};
        for (const auto &registersForCase : registers)
        {
            const auto fs = registersForCase[0], ft = registersForCase[1], fd = registersForCase[2];
            const auto is = registersForCase[3], it = registersForCase[4];
            for (uint32_t upperCode = 0; upperCode < 192u; ++upperCode)
            {
                for (uint32_t lowerCode = 0; lowerCode < 320u; ++lowerCode)
                {
                    // All upper and lower selector families are paired. Every
                    // lane mask is tested separately below; maximal masks here
                    // prove the capacity bound without sampling opcodes.
                    const uint32_t hiBase = upperCode < 64u
                        ? upper(upperCode, 15u, ft, fs, fd)
                        : special(upperCode - 64u, 15u, ft, fs);
                    const uint32_t lo = lowerCode < 128u
                        ? (lowerCode << 25u) | (15u << 21u) | (it << 16u) | (is << 11u)
                        : lowerCode < 192u
                            ? 0x80000000u | (15u << 21u) | (it << 16u) | (is << 11u) | (5u << 6u) | (lowerCode - 128u)
                            : lowerSpecial(lowerCode - 192u, is, it, 15u);
                    for (const uint32_t controlBits : {0u, 0x78000000u, 0x80000000u, 0xf8000000u})
                    {
                        const uint32_t hi = hiBase | controlBits;
                        candidate->verifyDecodePair(hi, lo, observed.data());
                        reference->verifyDecodePair(hi, lo, expected.data());
                        if (negativeControl && pairs == 0u)
                            observed[16] ^= 1u; // One actual candidate read-slot output.
                        if (observed != expected)
                            throw std::runtime_error("MISMATCH call=" + std::to_string(pairs) + " category=decoded-metadata");
                        if (observed[5] > 17u || observed[6] > 9u)
                            throw std::runtime_error("MISMATCH category=decoded-capacity");
                        maxReads = std::max(maxReads, observed[5]);
                        maxWrites = std::max(maxWrites, observed[6]);
                        ++pairs;
                    }
                }
                // Partial masks and nonmatching upper/lower masks preserve
                // exact slot order, not merely the maximum deadline value.
                for (uint32_t lowerCode = 0; lowerCode < 128u; ++lowerCode)
                    for (const uint32_t mask : masks)
                    {
                        const uint32_t hi = upperCode < 64u ? upper(upperCode, mask, ft, fs, fd)
                            : special(upperCode - 64u, mask, ft, fs);
                        const uint32_t lo = lowerSpecial(lowerCode, is, it, mask ^ 15u);
                        candidate->verifyDecodePair(hi, lo, observed.data());
                        reference->verifyDecodePair(hi, lo, expected.data());
                        if (observed != expected)
                            throw std::runtime_error("MISMATCH call=" + std::to_string(pairs) + " category=decoded-metadata");
                        ++pairs;
                    }
            }
        }
    }
    if (maxReads != 17u || maxWrites != 9u)
        throw std::runtime_error("Descriptor bound sentinels did not reach expected maxima");
    std::cout << "PASS decoded-pairs=" << pairs << " max-read-slots=" << maxReads
              << " max-write-slots=" << maxWrites
              << " candidate-pair-bytes=" << VU1Interpreter::verifyDecodedSize()
              << " reference-pair-bytes=" << VU1InterpreterRef::verifyDecodedSize() << '\n';
}
