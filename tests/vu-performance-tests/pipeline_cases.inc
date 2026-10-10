// Source-only differential cases for occupied-slot reuse and flag visibility.
void pipelineSynthetic()
{
    Pair p;
    auto fsset = [](uint32_t value) {
        return (0x15u << 25) | (((value >> 11) & 1u) << 21) | (value & 0x7ffu);
    };
    // Identity actions preserve arbitrary public high STATUS bits. FMAC and
    // FSSET actions retain the reference masks, and Q still commits afterward.
    for (uint32_t status : {0u,0xffffffffu,0xdeadffffu,0x80000fc0u,0x12345678u})
    {
        for (uint32_t mask = 0; mask < 16; ++mask)
        {
            p.reset(); p.seed(status ^ mask);
            p.c->state().status = p.r->state().status = status;
            p.instruction(0, (0x11u << 25) | 0xabcdefu, special(0x1fu,0,3,2));
            p.instruction(8, 0, nop); p.instruction(16, 0, nop);
            p.instruction(24, 0, nop); p.instruction(32, 0, nop | ebit);
            p.instruction(40, 0, nop);
            p.run(false,1); for (uint32_t cycle = 0; cycle < 12; ++cycle) p.run(true,1);
            p.c->state().status = p.r->state().status = status;
            p.instruction(0, fsset(0x980u), upper(0x29u,mask));
            p.instruction(8, lowerSpecial(0x38u,2,3), upper(0x2au,mask));
            p.instruction(16, 0, upper(0x28u,mask));
            p.instruction(24, 0, nop); p.instruction(32, 0, nop | ebit);
            p.run(false,1); for (uint32_t cycle = 0; cycle < 24; ++cycle) p.run(true,1);
        }
    }
    for (uint32_t mask = 0; mask < 16; ++mask)
    {
        for (uint32_t budget = 1; budget <= 12; ++budget)
        {
            p.reset(); p.seed(0x5e001u + mask * 100 + budget);
            // Two entries each cycle fill all eight slots. Subsequent cycles
            // repeatedly claim the lowest slots freed at four-cycle commits.
            for (uint32_t i = 0; i < 40; ++i)
            {
                if (i & 1u)
                    p.instruction(i * 8, (0x11u << 25) | ((i * 1337) & 0xffffffu),
                                  special(0x1fu, 0, 3, 2)); // FCSET cancels CLIP.
                else
                    p.instruction(i * 8, fsset((i * 0x140u) & 0xfc0u),
                                  upper(0x28u, mask)); // FSSET cancels STATUS, keeps MAC.
            }
            p.instruction(320, 0, nop | ebit); p.instruction(328, 0, nop);
            p.run(false, budget);
            for (uint32_t step = 0; step < 50; ++step) p.run(true, 1);
        }
    }
    for (uint32_t budget = 1; budget <= 16; ++budget)
    {
        p.reset(); p.seed(0x123400u + budget);
        // DIV and a later FMAC/FSSET mature on the same cycle; Q's D/I update
        // must follow flag slots exactly as in the current39 reference.
        p.instruction(0, lowerSpecial(0x38u, 2, 3), nop);
        p.instruction(8, 0, upper(0x28)); p.instruction(16, 0, upper(0x2a));
        p.instruction(24, fsset(0xc80), upper(0x28));
        p.instruction(32, lowerSpecial(0x3bu, 0), nop);
        p.instruction(40, lowerSpecial(0x70u, 2), special(0x1fu, 0, 3, 2));
        p.instruction(48, lowerSpecial(0x72u, 2), upper(0x28));
        p.instruction(56, lowerSpecial(0x7bu, 0), nop);
        p.instruction(64, 0, nop | ebit); p.instruction(72, 0, nop);
        p.run(false, budget);
        for (uint32_t step = 0; step < 45; ++step) p.run(true, 1);
        // Execute discards pending entries, while resume preserves them.
        p.instruction(0, fsset(0x980), upper(0x28)); p.run(false, 1);
        p.instruction(0, 0, nop | ebit); p.instruction(8, 0, nop);
        p.run(false, 1); p.run(true, 1);
    }
    // Partial stores must retain unselected words after slot reuse and reset.
    for (uint32_t mask = 0; mask < 16; ++mask)
    {
        p.reset(); p.seed(0xabe000u + mask);
        for (uint32_t i = 0; i < 24; ++i)
            p.instruction(i * 8, (1u << 25) | (mask << 21) | (1u << 16) | (2u << 11),
                          upper(0x28, mask));
        p.instruction(192, 0, nop | ebit); p.instruction(200, 0, nop);
        p.run(false, 1);
        for (uint32_t step = 0; step < 30; ++step) p.run(true, 1);
    }
}
