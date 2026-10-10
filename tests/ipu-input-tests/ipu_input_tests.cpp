#include "runtime/ps2_memory.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
unsigned checks = 0;
constexpr uint32_t channel = 0x1000B400u;
constexpr uint32_t madr = channel + 0x10u;
constexpr uint32_t qwc = channel + 0x20u;
constexpr uint32_t tadr = channel + 0x30u;
constexpr uint32_t dstat = 0x1000E010u;
void require(bool result, const char *message)
{
    ++checks;
    if (!result) throw std::runtime_error(message);
}
void tag(PS2Memory &memory, uint32_t at, unsigned id, unsigned words, uint32_t address, bool irq = false)
{
    memory.write64(at, static_cast<uint64_t>(words) | (static_cast<uint64_t>(id) << 28u) |
        (irq ? 0x80000000ull : 0ull) | (static_cast<uint64_t>(address) << 32u));
    memory.write64(at + 8u, 0u);
}
void startNormal(PS2Memory &memory, uint32_t source, unsigned words)
{
    memory.write32(channel, 1u);
    memory.write32(madr, source);
    memory.write32(qwc, words);
    memory.write32(channel, 0x101u);
}
void startChain(PS2Memory &memory, uint32_t first, uint32_t chcr = 0x105u)
{
    memory.write32(channel, 5u);
    memory.write32(qwc, 0u);
    memory.write32(tadr, first);
    memory.write32(channel, chcr);
}
void expectIrq(PS2Memory &memory, unsigned count)
{
    const auto causes = memory.consumeCompletedDmacCauses();
    require(causes.size() == count, "exact number of DMA completions");
    require(std::all_of(causes.begin(), causes.end(), [](auto cause) { return cause == 4u; }), "only IPU-input DMA IRQ4");
}
void normalAndFractional(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    memory.write32(dstat, 0x00100010u);
    startNormal(memory, 0x10000u, 4u);
    require((memory.read32(channel) & 0x100u) != 0u, "CHCR reads retain active IPU input");
    memory.creditIpuInputBytes(15u);
    require(memory.read32(madr) == 0x10000u && memory.read32(qwc) == 4u, "fractional credit does not retire a quadword");
    expectIrq(memory, 0u);
    memory.creditIpuInputBytes(1u);
    require(memory.read32(madr) == 0x10010u && memory.read32(qwc) == 3u, "sixteen-byte credit advances MADR and QWC");
    memory.creditIpuInputBytes(48u);
    require(memory.read32(madr) == 0x10040u && memory.read32(qwc) == 0u, "normal DMA retires exactly its payload");
    require((memory.read32(channel) & 0x100u) == 0u, "normal DMA clears STR after all credited payload");
    require((memory.read32(dstat) & 0x80000010u) == 0x80000010u, "completion publishes D_STAT status and enabled interrupt");
    expectIrq(memory, 1u);
    memory.creditIpuInputBytes(0u);
    memory.read32(channel);
    expectIrq(memory, 0u);
}
void creditBeforeSubmitAndReset(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    memory.creditIpuInputBytes(32u);
    expectIrq(memory, 0u);
    startNormal(memory, 0x11000u, 2u);
    require(memory.read32(madr) == 0x11020u && memory.read32(qwc) == 0u, "accepted bytes credited before guest submission are consumed once");
    expectIrq(memory, 1u);
    memory.creditIpuInputBytes(32u);
    memory.resetIpuInputConsumption();
    expectIrq(memory, 0u);
    startNormal(memory, 0x12000u, 2u);
    require(memory.read32(madr) == 0x12000u && memory.read32(qwc) == 2u, "new stream cannot inherit old consumption credit");
    memory.resetIpuInputConsumption();
    require((memory.read32(channel) & 0x100u) == 0u, "reset stops pending input DMA without an interrupt");
    expectIrq(memory, 0u);
}
void retailBlocksAndWrap(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    constexpr uint32_t tags = 0x20000u, input = 0x30000u;
    tag(memory, tags, 3u, 128u, input);
    tag(memory, tags + 16u, 0u, 128u, input + 2048u);
    startChain(memory, tags);
    memory.creditIpuInputBytes(32u);
    require(memory.read32(madr) == input + 32u && memory.read32(qwc) == 126u, "REF is partially consumed by actual accepted bytes");
    require(memory.read32(tadr) == tags + 16u, "REF TADR advances to following tag");
    require((memory.read32(channel) >> 28u & 7u) == 3u, "CHCR exposes fetched REF tag id");
    memory.creditIpuInputBytes(2016u);
    require(memory.read32(madr) == input + 2048u && memory.read32(qwc) == 128u, "one complete retail 2048-byte block advances to next submitted block");
    expectIrq(memory, 0u);
    memory.creditIpuInputBytes(2048u);
    require(memory.read32(madr) == input + 4096u && memory.read32(qwc) == 0u, "REFE completes second retail block");
    require(memory.read32(tadr) == tags + 32u && (memory.read32(channel) & 0x100u) == 0u, "REFE terminates chain and preserves final address");
    expectIrq(memory, 1u);

    // The actual movie uses a NEXT sentinel to wrap its circular tag table.
    memory.resetIpuInputConsumption();
    tag(memory, tags, 0u, 128u, input);
    tag(memory, tags + 16u, 3u, 128u, input + 2048u);
    tag(memory, tags + 32u, 2u, 0u, tags);
    startChain(memory, tags + 16u);
    memory.creditIpuInputBytes(4096u);
    require(memory.read32(madr) == input + 2048u && memory.read32(qwc) == 0u, "NEXT sentinel wraps to ring block zero without charging its tag bytes");
    require(memory.read32(tadr) == tags + 16u, "wrapped terminal REF preserves next-tag address");
    expectIrq(memory, 1u);
    // Register progress supplies the retail ring's own block retirement formula.
    const uint32_t block = (memory.read32(madr) - input) >> 11u;
    require(block == 1u, "retail MADR-to-block conversion observes wrapped consumed block");
}
void stopResumeAndRejectedBytes(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    tag(memory, 0x21000u, 0u, 4u, 0x40000u);
    startChain(memory, 0x21000u);
    memory.creditIpuInputBytes(16u);
    memory.write32(channel, 5u);
    memory.creditIpuInputBytes(32u);
    require(memory.read32(madr) == 0x40010u && memory.read32(qwc) == 3u, "STOP prevents progress despite later accepted-byte credit");
    expectIrq(memory, 0u);
    memory.write32(channel, 0x105u);
    require(memory.read32(madr) == 0x40030u && memory.read32(qwc) == 1u, "resume preserves partially loaded terminal tag and existing credit");
    expectIrq(memory, 0u);
    // A rejected callback contributes no credit: even repeated register polls
    // cannot complete or advance the unread final quadword.
    for (unsigned poll = 0u; poll < 40u; ++poll)
        require(memory.read32(madr) == 0x40030u && memory.read32(qwc) == 1u && (memory.read32(channel) & 0x100u), "uncredited/rejected bytes never drain input");
    memory.creditIpuInputBytes(16u);
    require(memory.read32(madr) == 0x40040u && (memory.read32(channel) & 0x100u) == 0u, "final accepted quadword completes resumed REFE");
    expectIrq(memory, 1u);
}
void retagPartialTerminal(PS2Memory &memory)
{
    // SDK movie rings extend a suspended tail by changing REFE to REF and
    // publishing that same tag ID in CHCR. MADR/QWC/TADR need no rewrite.
    constexpr uint32_t tags = 0x27000u, input = 0x70000u;
    for (const bool delayedEnable : {false, true})
    {
        memory.resetIpuInputConsumption();
        tag(memory, tags, 0u, 2u, input);
        tag(memory, tags + 16u, 0u, 2u, input + 32u);
        startChain(memory, tags);
        memory.creditIpuInputBytes(16u);
        require(memory.read32(madr) == input + 16u && memory.read32(qwc) == 1u,
            "extension starts with a partially consumed terminal tag");
        require(memory.read32(tadr) == tags + 16u, "partial tail retains its successor TADR");
        expectIrq(memory, 0u);
        memory.write32(channel, memory.read32(channel) & ~0x100u);
        tag(memory, tags, 3u, 2u, input);
        if (delayedEnable) memory.write32(0x1000E000u, 0u);
        memory.write32(channel, 0x30000105u);
        for (unsigned poll = 0u; poll < 8u; ++poll)
            require(memory.read32(madr) == input + 16u && memory.read32(qwc) == 1u &&
                (memory.read32(channel) & 0x100u) != 0u,
                "CHCR retagging never consumes uncredited input");
        expectIrq(memory, 0u);
        memory.creditIpuInputBytes(48u);
        if (delayedEnable)
        {
            require(memory.read32(madr) == input + 16u && memory.read32(qwc) == 1u,
                "disabled DMAC holds accepted credit across a CHCR retag");
            expectIrq(memory, 0u);
            memory.write32(0x1000E000u, 1u);
        }
        require(memory.read32(madr) == input + 64u && memory.read32(qwc) == 0u,
            "resumed REF follows its successor instead of the cached terminal state");
        require(memory.read32(tadr) == tags + 32u && (memory.read32(channel) & 0x100u) == 0u,
            "extended ring retires exactly the new terminal tag");
        expectIrq(memory, 1u);

        // All 48 accepted bytes went to the tail and successor. A later
        // transfer cannot inherit fabricated or double-counted input credit.
        startNormal(memory, input + 128u, 1u);
        require(memory.read32(madr) == input + 128u && memory.read32(qwc) == 1u,
            "ring extension leaves no fabricated credit for another transfer");
        expectIrq(memory, 0u);
        memory.creditIpuInputBytes(15u);
        require(memory.read32(qwc) == 1u, "fractional credit remains fractional after a ring extension");
        expectIrq(memory, 0u);
        memory.creditIpuInputBytes(1u);
        require(memory.read32(madr) == input + 144u && memory.read32(qwc) == 0u,
            "only the final real byte retires the later quadword");
        expectIrq(memory, 1u);
    }
}
void retagPartialNonterminal(PS2Memory &memory)
{
    constexpr uint32_t tags = 0x28000u, input = 0x71000u;
    for (const uint32_t terminalId : {0u, 7u})
    {
        memory.resetIpuInputConsumption();
        tag(memory, tags, 3u, 2u, input);
        tag(memory, tags + 16u, 0u, 2u, input + 32u);
        startChain(memory, tags);
        memory.creditIpuInputBytes(16u);
        memory.write32(channel, memory.read32(channel) & ~0x100u);
        memory.write32(channel, (terminalId << 28u) | 0x105u);
        require(memory.read32(madr) == input + 16u && memory.read32(qwc) == 1u,
            "restored terminal CHCR preserves unread payload");
        expectIrq(memory, 0u);
        memory.creditIpuInputBytes(16u);
        require(memory.read32(madr) == input + 32u && memory.read32(qwc) == 0u &&
            (memory.read32(channel) & 0x100u) == 0u,
            "REF resumed as REFE or END completes after its current payload");
        require(memory.read32(tadr) == tags + 16u, "new terminal CHCR does not fetch its queued successor");
        expectIrq(memory, 1u);
    }
}
void resumeUsesCurrentTieAndIrq(PS2Memory &memory)
{
    constexpr uint32_t tags = 0x29000u, input = 0x72000u;
    for (const bool enableTieOnResume : {false, true})
    {
        memory.resetIpuInputConsumption();
        tag(memory, tags, 3u, 2u, input, true);
        tag(memory, tags + 16u, 0u, 2u, input + 32u);
        startChain(memory, tags, enableTieOnResume ? 0x105u : 0x185u);
        memory.creditIpuInputBytes(16u);
        const uint32_t fetched = memory.read32(channel);
        require((fetched & 0xF0000000u) == 0xB0000000u, "fetched CHCR exposes the REF tag IRQ bit");
        memory.write32(channel, fetched & ~0x100u);
        const uint32_t resumed = enableTieOnResume ? fetched | 0x80u : fetched & ~0x80u;
        memory.write32(channel, resumed);
        require(memory.read32(madr) == input + 16u && memory.read32(qwc) == 1u,
            "changing TIE cannot itself earn input credit");
        expectIrq(memory, 0u);
        memory.creditIpuInputBytes(16u);
        if (enableTieOnResume)
        {
            require(memory.read32(qwc) == 0u && memory.read32(madr) == input + 32u &&
                (memory.read32(channel) & 0x100u) == 0u,
                "enabling TIE on resume honors the current CHCR IRQ bit");
            require(memory.read32(tadr) == tags + 16u, "resumed tag IRQ prevents successor fetch");
            expectIrq(memory, 1u);
        }
        else
        {
            require(memory.read32(qwc) == 2u && memory.read32(madr) == input + 32u &&
                (memory.read32(channel) & 0x100u) != 0u,
                "disabling TIE on resume continues past the old IRQ terminal state");
            expectIrq(memory, 0u);
            memory.creditIpuInputBytes(32u);
            require(memory.read32(madr) == input + 64u && memory.read32(qwc) == 0u,
                "noninterrupting resumed REF consumes its real terminal successor");
            expectIrq(memory, 1u);
        }
    }
}
void zeroQwcStartsAtTadr(PS2Memory &memory)
{
    constexpr uint32_t tags = 0x2A000u, input = 0x73000u;
    for (const uint32_t staleTag : {0u, 0x70000000u, 0xB0000080u})
    {
        memory.resetIpuInputConsumption();
        tag(memory, tags, 0u, 1u, input);
        startChain(memory, tags, staleTag | 0x105u);
        require(memory.read32(madr) == input && memory.read32(qwc) == 1u &&
            memory.read32(tadr) == tags + 16u,
            "zero-QWC resume fetches TADR despite an old terminal or IRQ CHCR tag");
        require((memory.read32(channel) & 0xF0000000u) == 0u,
            "fresh TADR tag replaces stale CHCR terminal and IRQ bits");
        expectIrq(memory, 0u);
        memory.creditIpuInputBytes(16u);
        require(memory.read32(madr) == input + 16u && memory.read32(qwc) == 0u,
            "fresh zero-QWC chain consumes only its accepted payload");
        expectIrq(memory, 1u);
    }
}
void tagInterruptsAndMalformedChain(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    tag(memory, 0x22000u, 3u, 1u, 0x50000u, true);
    tag(memory, 0x22010u, 0u, 1u, 0x50010u);
    startChain(memory, 0x22000u, 0x185u);
    memory.creditIpuInputBytes(16u);
    require(memory.read32(madr) == 0x50010u && memory.read32(qwc) == 0u && (memory.read32(channel) & 0x100u) == 0u, "TIE stops on tag IRQ after credited payload");
    expectIrq(memory, 1u);
    memory.resetIpuInputConsumption();
    tag(memory, 0x22000u, 2u, 0u, 0x22000u);
    startChain(memory, 0x22000u);
    memory.creditIpuInputBytes(32u);
    require((memory.read32(channel) & 0x100u) != 0u && memory.read32(qwc) == 0u, "zero-payload cyclic tags remain stalled instead of fabricating completion");
    expectIrq(memory, 0u);
    memory.resetIpuInputConsumption();
    tag(memory, 0x22000u, 0u, 2u, PS2_RAM_SIZE - 16u);
    startChain(memory, 0x22000u);
    const uint32_t beforeAddress = memory.read32(madr);
    const uint32_t beforeWords = memory.read32(qwc);
    memory.creditIpuInputBytes(32u);
    require(memory.read32(madr) == beforeAddress && memory.read32(qwc) == beforeWords && (memory.read32(channel) & 0x100u), "out-of-range DMA payload cannot retire credited bytes");
    expectIrq(memory, 0u);
}
void inlineTagsAndScratchpad(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    // CALL's return points past inline data; RET consumes its own data before
    // restoring that saved address, and END consumes its inline final data.
    tag(memory, 0x23000u, 5u, 1u, 0x24000u);
    tag(memory, 0x23020u, 7u, 1u, 0u);
    tag(memory, 0x24000u, 6u, 1u, 0u);
    startChain(memory, 0x23000u);
    memory.creditIpuInputBytes(16u);
    require(memory.read32(madr) == 0x24010u && memory.read32(tadr) == 0x23020u, "CALL and RET preserve inline payload and saved return tag");
    require(memory.read32(channel + 0x40u) == 0x23020u && (memory.read32(channel) & 0x30u) == 0u, "CALL saves ASR0 and RET restores ASP");
    memory.creditIpuInputBytes(32u);
    require(memory.read32(madr) == 0x23040u && memory.read32(tadr) == 0x23040u, "END consumes final inline data");
    expectIrq(memory, 1u);
    memory.resetIpuInputConsumption();
    tag(memory, 0x25000u, 1u, 1u, 0u);
    tag(memory, 0x25020u, 0u, 1u, 0x80003FF0u);
    startChain(memory, 0x25000u);
    memory.creditIpuInputBytes(16u);
    require(memory.read32(madr) == 0x80003FF0u && memory.read32(qwc) == 1u, "CNT advances over inline payload to scratchpad REF");
    memory.creditIpuInputBytes(16u);
    require(memory.read32(madr) == 0x80004000u && memory.read32(qwc) == 0u, "scratchpad DMAC address bit31 is preserved through retirement");
    expectIrq(memory, 1u);
}
void disabledControllerAndHardwareReset(PS2Memory &memory)
{
    memory.resetIpuInputConsumption();
    memory.write32(0x1000E000u, 0u);
    startNormal(memory, 0x60000u, 2u);
    memory.creditIpuInputBytes(32u);
    require(memory.read32(madr) == 0x60000u && memory.read32(qwc) == 2u, "disabled DMAC preserves credited input until enabled");
    expectIrq(memory, 0u);
    memory.write32(0x1000E000u, 1u);
    require(memory.read32(madr) == 0x60020u && memory.read32(qwc) == 0u, "DMAC enable releases only previously credited payload");
    expectIrq(memory, 1u);
    memory.creditIpuInputBytes(16u);
    memory.write32(0x10002010u, 0x40000000u);
    startNormal(memory, 0x61000u, 1u);
    require(memory.read32(madr) == 0x61000u && memory.read32(qwc) == 1u, "IPU_CTRL reset clears old stream-generation credit");
    expectIrq(memory, 0u);
    memory.resetIpuInputConsumption();
}
}
int main()
{
    try
    {
        PS2Memory memory;
        require(memory.initialize(), "real PS2Memory initializes");
        normalAndFractional(memory);
        creditBeforeSubmitAndReset(memory);
        retailBlocksAndWrap(memory);
        stopResumeAndRejectedBytes(memory);
        retagPartialTerminal(memory);
        retagPartialNonterminal(memory);
        resumeUsesCurrentTieAndIrq(memory);
        zeroQwcStartsAtTadr(memory);
        tagInterruptsAndMalformedChain(memory);
        inlineTagsAndScratchpad(memory);
        disabledControllerAndHardwareReset(memory);
#ifdef IPU_RESUME_OLD_CONTROL
        throw std::runtime_error("old IPU resume control did not reproduce stale terminal classification");
#endif
        std::cout << "kfiv_ipu_input_regression: " << checks << " checks, 0 failures\n";
        return 0;
    }
    catch (const std::exception &error)
    {
#ifdef IPU_RESUME_OLD_CONTROL
        if (std::string(error.what()) ==
            "resumed REF follows its successor instead of the cached terminal state")
        {
            std::cout << "kfiv_ipu_input_old_control: expected stale terminal classification after "
                << checks << " checks\n";
            return 0;
        }
#endif
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
