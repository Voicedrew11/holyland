// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kVif0 = 0x10008000u;
constexpr uint32_t kVif1 = 0x10009000u;
constexpr uint32_t kGif = 0x1000a000u;
constexpr uint32_t kCsr = 0x12001000u;
constexpr uint32_t kHead = 0x10000u;
constexpr uint32_t kMscal = 0x14000276u;
int checks = 0;
int failures = 0;
std::string currentCase;

void check(bool value, const char *description)
{
    ++checks;
    if (!value)
    {
        ++failures;
        std::cerr << "FAIL " << currentCase << ": " << description << '\n';
    }
}

struct Fixture
{
    PS2Memory memory;
    GS gs;
    uint32_t packets = 0;
    uint32_t mscalCalls = 0;
    bool mscalArgumentsValid = true;
    GifArbiter arbiter;

    Fixture() : arbiter([this](const uint8_t *data, uint32_t bytes)
                           { ++packets; gs.processGIFPacket(data, bytes); })
    {
        if (!memory.initialize())
            throw std::runtime_error("PS2Memory initialization failed");
        gs.init(memory.getGSVRAM(), PS2_GS_VRAM_SIZE, &memory.gs());
        memory.setGifArbiter(&arbiter);
        memory.setVu1MscalCallback([this](uint32_t pc, uint32_t top, uint32_t itop)
            { ++mscalCalls; mscalArgumentsValid &= pc == 0x276u * 8u && top == 0u && itop == 0u; });
        memory.write64(kCsr, 2u); // W1C: each case starts with FINISH absent.
    }
};

void tag(PS2Memory &memory, uint32_t at, uint32_t id, uint16_t qwc,
         uint32_t address = 0u, bool irq = false, uint64_t upper = 0u)
{
    memory.write64(at, qwc | (static_cast<uint64_t>(id) << 28u) |
        (irq ? 0x80000000ull : 0ull) | (static_cast<uint64_t>(address) << 32u));
    memory.write64(at + 8u, upper);
}

void start(PS2Memory &memory, uint32_t channel, uint32_t head, uint32_t chcr = 0x105u)
{
    memory.write32(channel + 0x00u, 5u);
    memory.write32(channel + 0x20u, 0u);
    memory.write32(channel + 0x30u, head);
    memory.write32(channel + 0x40u, 0u);
    memory.write32(channel + 0x50u, 0u);
    memory.write32(channel + 0x00u, chcr);
}

void gifFinish(PS2Memory &memory, uint32_t at)
{
    // One EOP PACKED A_D register, NREG=1, followed by GS FINISH.
    memory.write64(at, (1ull << 60u) | 0x8001u);
    memory.write64(at + 8u, 0x0eu);
    memory.write64(at + 16u, 0xffffffffu);
    memory.write64(at + 24u, 0x61u);
}

void vifFinish(PS2Memory &memory, uint32_t at)
{
    memory.write64(at, 0u);
    memory.write32(at + 8u, 0u);
    memory.write32(at + 12u, 0x50000002u); // DIRECT, two GIF quadwords.
    gifFinish(memory, at + 16u);
}

void completion(Fixture &f, uint32_t channel, uint32_t cause)
{
    check((f.memory.read32(channel) & 0x100u) == 0u, "DMA STR cleared");
    check(f.memory.read32(channel + 0x20u) == 0u, "DMA QWC retired");
    check((f.memory.read32(0x1000e010u) & (1u << cause)) != 0u, "D_STAT channel completion set");
    const auto causes = f.memory.consumeCompletedDmacCauses();
    check(causes == std::vector<uint32_t>{cause}, "one scheduler DMA completion cause");
}

void finish(Fixture &f)
{
    check(f.packets == 1u, "exactly one GIF packet reaches actual frontend");
    check((f.memory.read64(kCsr) & 2u) != 0u, "GS FINISH visible through actual CSR MMIO");
    f.memory.write64(kCsr, 2u);
    check((f.memory.read64(kCsr) & 2u) == 0u, "GS FINISH acknowledged by actual W1C MMIO");
}

void finiteVif(uint32_t count, bool mixed, bool tte, bool separateEnd = false)
{
    currentCase = std::string(tte ? "VIF1 TTE/NEXT " : mixed ? "VIF1 CNT/NEXT " : "VIF1 CNT ") +
        std::to_string(count) + (separateEnd ? " with separate zero-QWC END" : "");
    Fixture f;
    uint32_t at = kHead;
    const uint32_t fillers = count - (separateEnd ? 2u : 1u);
    for (uint32_t index = 0u; index < fillers; ++index)
    {
        const bool next = tte || (mixed && (index & 1u));
        const uint16_t qwc = tte ? 0u : 1u;
        const uint32_t after = at + (next ? (tte ? 32u : 64u) : 32u);
        tag(f.memory, at, next ? 2u : 1u, qwc, next ? after : 0u,
            false, tte ? static_cast<uint64_t>(kMscal) << 32u : 0u);
        if (!tte)
        {
            f.memory.write32(at + 16u, kMscal);
            f.memory.write32(at + 20u, 0u);
            f.memory.write64(at + 24u, 0u);
        }
        if (next)
            tag(f.memory, at + (tte ? 16u : 32u), 0u, 0u); // Unreachable early stop.
        at = after;
    }
    tag(f.memory, at, separateEnd ? 1u : 7u, 3u, 0u, false,
        tte ? static_cast<uint64_t>(kMscal) << 32u : 0u);
    vifFinish(f.memory, at + 16u);
    const uint32_t terminal = separateEnd ? at + 64u : at;
    if (separateEnd)
        tag(f.memory, terminal, 7u, 0u);
    start(f.memory, kVif1, kHead, tte ? 0x145u : 0x105u);
    finish(f);
    completion(f, kVif1, 1u);
    check(f.memory.read32(kVif1 + 0x30u) == terminal, "TADR reaches terminal END");
    check(f.mscalCalls == (tte ? count : fillers), "every source-tag MSCAL reaches real VIF callback");
    check(f.mscalArgumentsValid, "MSCAL address/TOP/ITOP preserved");
}

void finiteGif(uint32_t count)
{
    currentCase = "GIF NEXT " + std::to_string(count);
    Fixture f;
    for (uint32_t index = 0; index + 1u < count; ++index)
        tag(f.memory, kHead + index * 32u, 2u, 0u, kHead + (index + 1u) * 32u);
    const uint32_t terminal = kHead + (count - 1u) * 32u;
    tag(f.memory, terminal, 7u, 2u);
    gifFinish(f.memory, terminal + 16u);
    start(f.memory, kGif, kHead);
    finish(f);
    completion(f, kGif, 2u);
    check(f.memory.read32(kGif + 0x30u) == terminal, "TADR reaches GIF END");
}

void vif0Tail()
{
    currentCase = "VIF0 NEXT 10000";
    Fixture f;
    for (uint32_t index = 0u; index < 9999u; ++index)
        tag(f.memory, kHead + index * 16u, 2u, 0u, kHead + (index + 1u) * 16u);
    const uint32_t terminal = kHead + 9999u * 16u;
    tag(f.memory, terminal, 7u, 1u);
    f.memory.write32(terminal + 16u, 0x07005aa5u); // MARK.
    start(f.memory, kVif0, kHead);
    check(f.memory.vif0_regs.mark == 0x5aa5u, "tail MARK reaches actual VIF0");
    completion(f, kVif0, 0u);
    check(f.memory.read32(kVif0 + 0x30u) == terminal, "TADR reaches VIF0 END");
}

void repeatedCalls(bool nested)
{
    currentCase = nested ? "nested CALL/RET reuse" : "CALL/RET reuse";
    Fixture f;
    constexpr uint32_t calls = 5000u;
    constexpr uint32_t subroutine = 0x80000u;
    constexpr uint32_t inner = 0x81000u;
    for (uint32_t index = 0u; index < calls; ++index)
        tag(f.memory, kHead + index * 16u, 5u, 0u, subroutine);
    const uint32_t terminal = kHead + calls * 16u;
    tag(f.memory, terminal, 7u, 3u);
    vifFinish(f.memory, terminal + 16u);
    if (nested)
    {
        tag(f.memory, subroutine, 5u, 0u, inner);
        tag(f.memory, subroutine + 16u, 6u, 0u);
        tag(f.memory, inner, 6u, 1u);
        f.memory.write32(inner + 16u, kMscal);
    }
    else
    {
        tag(f.memory, subroutine, 6u, 1u);
        f.memory.write32(subroutine + 16u, kMscal);
    }
    start(f.memory, kVif1, kHead);
    finish(f);
    completion(f, kVif1, 1u);
    check(f.mscalCalls == calls, "same subroutine executes on every distinct return stack");
    check(f.memory.read32(kVif1 + 0x30u) == terminal, "CALL reuse reaches terminal END");
    check((f.memory.read32(kVif1) & 0x30u) == 0u, "CALL stack unwinds to ASP zero");
}

void irqTie(bool tie)
{
    currentCase = tie ? "IRQ with TIE" : "IRQ without TIE";
    Fixture f;
    tag(f.memory, kHead, 1u, 1u, 0u, true);
    f.memory.write32(kHead + 16u, kMscal);
    tag(f.memory, kHead + 32u, 7u, 3u);
    vifFinish(f.memory, kHead + 48u);
    start(f.memory, kVif1, kHead, tie ? 0x185u : 0x105u);
    check(f.mscalCalls == 1u, "IRQ tag payload executes before stop");
    check(f.packets == (tie ? 0u : 1u), "TIE controls delivery of subsequent FINISH");
    check(((f.memory.read64(kCsr) & 2u) != 0u) == !tie, "TIE FINISH outcome through actual CSR");
    check(f.memory.read32(kVif1 + 0x30u) == kHead + 32u, "TADR records next/terminal tag");
    check((f.memory.read32(kVif1) >> 16u) == (tie ? 0x9000u : 0x7000u), "CHCR.TAG preserves last executed tag");
    completion(f, kVif1, 1u);
}

void emptyEnd()
{
    currentCase = "zero-QWC END";
    Fixture f;
    tag(f.memory, kHead, 7u, 0u);
    start(f.memory, kVif1, kHead);
    check(f.memory.read32(kVif1 + 0x30u) == kHead, "empty END stops at its tag");
    check((f.memory.read32(kVif1) >> 16u) == 0x7000u, "empty END recorded as last tag");
    check(f.packets == 0u && f.mscalCalls == 0u, "empty END executes no payload");
    check((f.memory.read64(kCsr) & 2u) == 0u, "empty END produces no FINISH");
    // No pending transfer is queued for an entirely empty non-TTE chain.
    // Its pre-existing completion semantics are outside the traversal fix.
}

void cycle(bool twoTags, bool call)
{
    currentCase = call ? "cyclic CALL" : twoTags ? "two-tag NEXT cycle" : "self NEXT cycle";
    Fixture f;
    tag(f.memory, kHead, call ? 5u : 2u, 1u, twoTags ? kHead + 32u : kHead);
    f.memory.write32(kHead + 16u, kMscal);
    if (twoTags)
    {
        tag(f.memory, kHead + 32u, 2u, 1u, kHead);
        f.memory.write32(kHead + 48u, kMscal);
    }
    const auto before = std::chrono::steady_clock::now();
    start(f.memory, kVif1, kHead);
    check(std::chrono::steady_clock::now() - before < std::chrono::seconds(2), "cyclic input returns promptly");
    check(f.mscalCalls > 0u && f.mscalCalls < 16u, "state cycle guard bounds cyclic payload execution");
    check(f.packets == 0u && (f.memory.read64(kCsr) & 2u) == 0u, "cyclic input invents no FINISH");
    completion(f, kVif1, 1u);
}
}

int main()
{
    try
    {
        for (uint32_t count : {4095u, 4096u, 4097u, 4487u, 10000u})
        {
            finiteVif(count, false, false);
            finiteVif(count, true, false);
            finiteVif(count, false, true);
        }
        finiteVif(4487u, true, false, true);
        finiteGif(4097u);
        finiteGif(10000u);
        vif0Tail();
        repeatedCalls(false);
        repeatedCalls(true);
        irqTie(false);
        irqTie(true);
        emptyEnd();
        cycle(false, false);
        cycle(true, false);
        cycle(false, true);
    }
    catch (const std::exception &error)
    {
        std::cerr << "Exception in " << currentCase << ": " << error.what() << '\n';
        return 2;
    }
    std::cout << checks << " DMA chain checks; " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
