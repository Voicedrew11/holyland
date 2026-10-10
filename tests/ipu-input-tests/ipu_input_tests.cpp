#include "runtime/ps2_memory.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

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
        tagInterruptsAndMalformedChain(memory);
        inlineTagsAndScratchpad(memory);
        disabledControllerAndHardwareReset(memory);
        std::cout << "kfiv_ipu_input_regression: " << checks << " checks, 0 failures\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
