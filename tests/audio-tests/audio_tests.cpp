#include "emulator/core/iop_spu2.h"
#include "emulator/core/iop_memory.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

using ps2x::iop::detail::IopSpu2;
using ps2x::iop::detail::IopMemory;
namespace
{
    unsigned checks = 0;
    void check(bool condition, const char *message)
    {
        ++checks;
        if (!condition)
        {
            std::fprintf(stderr, "FAIL [%u]: %s\n", checks, message);
            std::exit(1);
        }
    }
    void addr(IopSpu2 &spu, unsigned core, unsigned offset, uint32_t word)
    {
        spu.write16(core * 0x400 + offset, static_cast<uint16_t>(word >> 16));
        spu.write16(core * 0x400 + offset + 2, static_cast<uint16_t>(word));
    }
    void upload(IopSpu2 &spu, unsigned core, uint32_t word, std::span<const uint8_t> source)
    {
        std::vector<uint8_t> bytes(source.begin(), source.end());
        addr(spu, core, 0x1a8, word);
        spu.dmaTransfer(core, true, bytes.data(), static_cast<uint32_t>(bytes.size() / 4));
        spu.completeDma(core);
    }
    std::vector<int16_t> render(IopSpu2 &spu, unsigned frames)
    {
        std::vector<int16_t> result;
        spu.setPcmCallback([&](std::span<const int16_t> chunk) { result.insert(result.end(), chunk.begin(), chunk.end()); });
        spu.advance(static_cast<uint64_t>(frames) * 768);
        spu.flushPcm();
        spu.setPcmCallback({});
        return result;
    }
    void route(IopSpu2 &spu, unsigned core)
    {
        const unsigned bank = core * 0x400;
        spu.write16(bank + 0x19a, 0xc000);
        spu.write16(bank + 0x188, 0xffff);
        spu.write16(bank + 0x18a, 0xff);
        spu.write16(bank + 0x190, 0xffff);
        spu.write16(bank + 0x192, 0xff);
        spu.write16(bank + 0x198, 0xc00);
        spu.write16(0x760 + core * 0x28, 0x3fff);
        spu.write16(0x762 + core * 0x28, 0x3fff);
    }
    void voice(IopSpu2 &spu, unsigned core, unsigned index, uint32_t word, uint16_t pitch = 0x1000)
    {
        const unsigned bank = core * 0x400 + index * 16;
        spu.write16(bank, 0x3fff);
        spu.write16(bank + 2, 0x3fff);
        spu.write16(bank + 4, pitch);
        spu.write16(bank + 6, 0x000f); // Fast attack, highest sustain level.
        spu.write16(bank + 8, 0x1f00); // Sustain increase rate 0, fast release.
        addr(spu, core, 0x1c0 + index * 12, word);
        spu.write16(core * 0x400 + 0x1a0 + (index >= 16 ? 2 : 0), static_cast<uint16_t>(1 << (index % 16)));
    }
    std::array<uint8_t, 16> adpcm(uint8_t flags = 7, uint8_t payload = 0x11)
    {
        std::array<uint8_t, 16> data;
        data.fill(payload);
        data[0] = 0;
        data[1] = flags;
        return data;
    }
    void clockAndReset()
    {
        IopSpu2 spu;
        unsigned samples = 0, calls = 0, irqs = 0;
        spu.setPcmCallback([&](std::span<const int16_t> pcm) { samples += static_cast<unsigned>(pcm.size()); ++calls; });
        spu.setIrqCallback([&] { ++irqs; });
        spu.advance(767);
        check(spu.statistics().frames == 0, "fractional clock does not emit early");
        spu.advance(1);
        check(spu.statistics().frames == 1, "one frame per 768 IOP cycles");
        spu.advance(255 * 768);
        check(samples == 512 && calls == 1, "256-frame even interleaved chunks");
        spu.reset();
        check(spu.statistics().frames == 0 && spu.read16(0x344) == 0 && spu.read16(0x744) == 0, "reset clears counters and DMA state");
        spu.advance(256 * 768);
        check(samples == 1024 && calls == 2, "reset preserves PCM callback");
        addr(spu, 0, 0x19c, 0x12345);
        spu.write16(0x19a, 0x8040);
        addr(spu, 1, 0x1a8, 0x12345);
        spu.write16(0x5ac, 0x9876);
        check(irqs == 1 && spu.read16(0x7c2) == 4, "reset preserves IRQ callback and other core PIO triggers IRQ");
        spu.write16(0x19a, 0x8000);
        check(spu.read16(0x7c2) == 0, "IRQ disable acknowledges IRQINFO");
    }
    void dmaAndPio()
    {
        IopSpu2 spu;
        spu.write16(0x19a, 0x8000);
        check(spu.read16(0x344) == 0, "library reset sees zero DMA status");
        spu.write16(0x19a, 0x8020);
        check((spu.read16(0x344) & 0x80) != 0, "DMA mode transition reports ready");
        std::array<uint8_t, 16> input{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}, output{};
        addr(spu, 0, 0x1a8, 0xffffc);
        spu.dmaTransfer(0, true, input.data(), 4);
        check((spu.read16(0x344) & 0x480) == 0x400, "DMA starts busy and not complete");
        check(spu.read16(0x1a8) == 0 && spu.read16(0x1aa) == 4, "TSA wraps in 20-bit word units");
        spu.completeDma(0);
        check((spu.read16(0x344) & 0x480) == 0x80, "completion clears busy and sets complete");
        spu.write16(0x19a, 0x8000);
        check((spu.read16(0x344) & 0x80) == 0, "DMA disable clears completion for interrupt polling");
        addr(spu, 1, 0x1a8, 0xffffc);
        spu.dmaTransfer(1, false, output.data(), 4);
        check(output == input, "both cores read the same 2 MiB RAM with wrap");
        check(spu.statistics().dmaReads == 1 && spu.statistics().dmaWrites == 1 && spu.statistics().dmaBytes == 32, "DMA direction and byte accounting");
        spu.completeDma(1);
        addr(spu, 0, 0x1a8, 0x98765);
        spu.write16(0x1ac, 0x55aa);
        spu.write16(0x1ac, 0xa55a);
        addr(spu, 1, 0x1a8, 0x98765);
        check(spu.read16(0x5ac) == 0x55aa && spu.read16(0x5ac) == 0xa55a, "PIO reads/writes increment halfword TSA");
        check(spu.read16(0x7ff) == 0 && spu.read16(0x800) == 0, "invalid MMIO offsets are bounded");
    }
    void adpcmPlayback()
    {
        IopSpu2 spu;
        route(spu, 1);
        const auto block = adpcm();
        upload(spu, 0, 0x40000, block);
        voice(spu, 1, 0, 0x40000);
        const auto pcm = render(spu, 80);
        check(pcm.size() == 160, "requested audio duration is preserved");
        check(pcm[40] > 3500 && pcm[40] < 4300 && pcm[41] == pcm[40], "raw ADPCM plus Gaussian/envelope/volume produces known stereo amplitude");
        check((spu.read16(0x740) & 1) != 0, "loop end sets ENDX");
        check(spu.read16(0x5c4) == 4 && spu.read16(0x5c6) == 0, "loop-start flag captures full 20-bit LSA");
        check(spu.statistics().decodedBlocks >= 3 && spu.statistics().keyOns == 1, "loop actually decodes repeatedly");
        spu.write16(0x740, 0xffff);
        check((spu.read16(0x740) & 1) == 0, "ENDX write clears selected half regardless value");
        spu.write16(0x5a4, 1);
        const auto release = render(spu, 8);
        check(spu.read16(0x40a) == 0 && release.back() == 0, "key-off follows programmed release and stops voice");

        spu.reset(); route(spu, 1);
        const auto once = adpcm(1);
        upload(spu, 0, 0x5000, once);
        voice(spu, 1, 17, 0x5000, 0x2000);
        render(spu, 10);
        check(spu.read16(0x51a) > 0, "high-half KON voice 17 still playing before its 28 source samples end");
        render(spu, 8);
        check(spu.read16(0x51a) == 0 && (spu.read16(0x742) & 2), "double pitch ends after fourteen frames and reports high-half ENDX");

        spu.reset(); route(spu, 1);
        const auto negative = adpcm(7, 0xff);
        upload(spu, 0, 0x5000, negative);
        voice(spu, 1, 0, 0x5000);
        const auto negativePcm = render(spu, 40);
        check(negativePcm[40] < -3500, "ADPCM negative nibbles sign extend");
        spu.write16(0x400, 0x4000); // Fixed volume negative phase (-32768).
        const auto phase = render(spu, 8);
        check(phase[10] > 3500 && phase[11] < -3500, "independent signed voice volume phase");
    }
    void routingNoiseAndSweep()
    {
        IopSpu2 spu;
        route(spu, 0); route(spu, 1);
        upload(spu, 1, 0x8000, adpcm());
        voice(spu, 0, 0, 0x8000);
        spu.write16(2, 0);
        spu.write16(0x598, 0x000c); // Core 1 dry external L/R.
        spu.write16(0x790, 0x7fff);
        spu.write16(0x792, 0x7fff);
        const auto pcm = render(spu, 40);
        check(pcm[40] > 3500 && pcm[41] == 0, "core 0 routes through core 1 external input without double mixing");
        spu.write16(0x598, 0);
        const auto muted = render(spu, 8);
        check(std::all_of(muted.begin(), muted.end(), [](int16_t v) { return v == 0; }), "MMIX gates external channels");

        spu.reset(); route(spu, 1);
        voice(spu, 1, 0, 0x8000);
        spu.write16(0x584, 1);
        spu.write16(0x59a, 0xff00); // Fast noise clock.
        const auto noise = render(spu, 256);
        check(std::any_of(noise.begin(), noise.end(), [](int16_t v) { return v != 0; }), "noise generator feeds selected voices");
        check(spu.statistics().nonzeroFrames != 0, "nonzero sample statistics cover noise");

        spu.reset(); route(spu, 1);
        upload(spu, 0, 0x8000, adpcm());
        voice(spu, 1, 0, 0x8000);
        spu.write16(0x400, 0);
        spu.write16(0x400, 0x8000); // Fast linear increasing sweep.
        render(spu, 1);
        check(spu.read16(0x40c) == 14336, "volume sweep changes current value rather than treating sweep bit as amplitude");
        render(spu, 3);
        check(spu.read16(0x40c) == 32767, "volume sweep reaches maximum");
    }
    void autoDma()
    {
        IopSpu2 spu;
        route(spu, 1);
        spu.write16(0x598, 0xc0);
        spu.write16(0x794, 0x7fff);
        spu.write16(0x796, 0x7fff);
        spu.write16(0x5b0, 2);
        std::vector<uint8_t> data(1024);
        for (unsigned i = 0; i != 256; ++i)
        {
            const int16_t left = 1000, right = -2000;
            std::memcpy(data.data() + i * 2, &left, 2);
            std::memcpy(data.data() + 512 + i * 2, &right, 2);
        }
        spu.dmaTransfer(1, true, data.data(), 256);
        check(spu.dmaCompletionCycles(1, 256) == 64, "single AutoDMA half completes when queued, with a positive bus-delay floor");
        const auto pcm = render(spu, 257);
        for (unsigned i = 0; i != 256; ++i)
        {
            check(pcm[i * 2] >= 997 && pcm[i * 2] <= 1000, "AutoDMA planar left");
            check(pcm[i * 2 + 1] >= -2000 && pcm[i * 2 + 1] <= -1997, "AutoDMA planar right");
        }
        check(pcm[512] == 0 && pcm[513] == 0, "AutoDMA underrun emits silence without replaying stale buffer");
        addr(spu, 0, 0x1a8, 0x2400);
        check(spu.read16(0x1ac) == 1000, "AutoDMA copies left input to core 1 hardware window");
        addr(spu, 0, 0x1a8, 0x2600);
        check(spu.read16(0x1ac) == static_cast<uint16_t>(-2000), "AutoDMA copies right input to core 1 hardware window");
        spu.completeDma(1);
        check((spu.read16(0x744) & 0x480) == 0x80, "AutoDMA completion status becomes ready");
    }
    void autoDmaRefillReserve()
    {
        IopSpu2 spu;
        route(spu, 1);
        spu.write16(0x598, 0xc0);
        spu.write16(0x794, 0x7fff); spu.write16(0x796, 0x7fff);
        spu.write16(0x5b0, 2);
        const auto planar = [](unsigned blocks, int16_t left, int16_t right)
        {
            std::vector<uint8_t> data(blocks * 1024);
            for (unsigned b = 0; b != blocks; ++b)
                for (unsigned f = 0; f != 256; ++f)
                {
                    std::memcpy(data.data() + b * 1024 + f * 2, &left, 2);
                    std::memcpy(data.data() + b * 1024 + 512 + f * 2, &right, 2);
                }
            return data;
        };
        auto first = planar(4, 1000, -2000), next = planar(4, 3000, -4000);
        spu.dmaTransfer(1, true, first.data(), 1024);
        const auto firstDelay = spu.dmaCompletionCycles(1, 1024);
        check(firstDelay == 768ull * 768, "initial fill deadline precedes the last 256-frame MEMIN half");
        const auto beforeIrq = render(spu, static_cast<unsigned>(firstDelay / 768));
        check(beforeIrq.size() == 768 * 2 && beforeIrq.back() < -1900, "actual samples play through the initial fill deadline");
        check((spu.read16(0x744) & 0x480) == 0x400, "fill deadline awaits the actual DMA completion signal");
        spu.completeDma(1);
        check((spu.read16(0x744) & 0x480) == 0x80, "DMA IRQ becomes ready while final MEMIN samples remain queued");
        // A real LIBSD handler consumes guest cycles before submitting the next
        // buffer. Its remaining 252 samples must still precede the new buffer.
        const auto handlerPcm = render(spu, 4);
        check(handlerPcm.front() > 990 && handlerPcm.back() < -1900, "guest IRQ-handler time consumes the genuine reserve without silence");
        spu.dmaTransfer(1, true, next.data(), 1024);
        const auto nextDelay = spu.dmaCompletionCycles(1, 1024);
        check(nextDelay == (252ull + 1024 - 256) * 768, "next DMA deadline includes prior unplayed tail instead of accelerating the loop");
        const auto oldTail = render(spu, 252);
        check(oldTail.front() > 990 && oldTail.back() < -1900, "appended buffer preserves all old-tail samples and channel order");
        const auto nextHead = render(spu, 768);
        check(nextHead.front() > 2990 && nextHead.back() < -3900, "new signed planar buffer follows reserve without replay or a gap");
        spu.completeDma(1);
        check((spu.read16(0x744) & 0x480) == 0x80, "next completion leaves another MEMIN half available");
        spu.write16(0x5b0, 0);
        const auto stopped = render(spu, 512);
        check(std::all_of(stopped.begin(), stopped.end(), [](int16_t v) { return v == 0; }), "ADMAS STOP discards the completed transfer's remaining reserve");
        spu.completeDma(1);
        check((spu.read16(0x744) & 0x480) == 0, "old completion cannot revive a stopped completed-transfer reserve");
        spu.write16(0x5b0, 2);
        auto one = planar(1, 5000, -6000);
        spu.dmaTransfer(1, true, one.data(), 256);
        check(spu.dmaCompletionCycles(1, 256) == 64, "fresh one-half restart has bounded positive completion delay");
        spu.completeDma(1);
        const auto restarted = render(spu, 257);
        check(restarted.front() > 4990 && restarted[511] < -5990 && restarted[512] == 0 && restarted[513] == 0,
              "completed restart plays only its genuine 256 frames and then emits silence");
        spu.reset();
        route(spu, 1); spu.write16(0x5b0, 2);
        uint8_t shortData[4] = {1, 0, 2, 0};
        spu.dmaTransfer(1, true, shortData, 1);
        check(spu.dmaCompletionCycles(1, 1) == 64, "partial MEMIN transfer cannot underflow the 256-frame lead");
    }
    void loopAndPmon()
    {
        IopSpu2 spu;
        route(spu, 1);
        std::array<uint8_t, 32> pair{};
        const auto first = adpcm(4), second = adpcm(3, 0x22);
        std::copy(first.begin(), first.end(), pair.begin());
        std::copy(second.begin(), second.end(), pair.begin() + 16);
        upload(spu, 0, 0x8000, pair);
        addr(spu, 1, 0x1c4, 0x9000);
        voice(spu, 1, 0, 0x8000);
        render(spu, 1);
        check(spu.read16(0x5c6) == 0x8000, "KON resets manual loop mode, preserving automatic loop-start semantics");
        addr(spu, 1, 0x1c4, 0x8008);
        render(spu, 80);
        check(spu.read16(0x5c6) == 0x8008, "LSA writes after KON override automatic loop-start capture");
        check((spu.read16(0x740) & 1) && spu.read16(0x40a) != 0, "manual loop target remains playing after ENDX");
        spu.write16(0x5a4, 1);
        spu.write16(0x5a0, 1);
        render(spu, 5);
        check(spu.read16(0x40a) != 0, "pending KON takes priority over pending KOFF");

        const auto measure = [&](bool pmon)
        {
            spu.reset(); route(spu, 1);
            upload(spu, 0, 0x8000, adpcm());
            voice(spu, 1, 0, 0x8000);
            voice(spu, 1, 1, 0x8000);
            if (pmon) spu.write16(0x580, 2);
            render(spu, 160);
            return spu.statistics().decodedBlocks;
        };
        const auto ordinary = measure(false), modulated = measure(true);
        check(modulated > ordinary, "PMON actually changes voice pitch using the preceding voice amplitude");

        spu.reset(); route(spu, 1);
        auto invalid = adpcm(); invalid[0] = 0x50;
        upload(spu, 0, 0x8000, invalid); voice(spu, 1, 0, 0x8000);
        const auto invalidPcm = render(spu, 64);
        spu.reset(); route(spu, 1);
        upload(spu, 0, 0x8000, adpcm()); voice(spu, 1, 0, 0x8000);
        check(invalidPcm == render(spu, 64), "reserved ADPCM predictors use zero coefficients");
    }
    void autoDmaStop()
    {
        IopSpu2 spu;
        route(spu, 1);
        spu.write16(0x598, 0xc0);
        spu.write16(0x794, 0x7fff); spu.write16(0x796, 0x7fff);
        spu.write16(0x5b0, 2);
        std::vector<uint8_t> bytes(4096);
        for (unsigned block = 0; block != 4; ++block)
            for (unsigned frame = 0; frame != 256; ++frame)
            {
                const int16_t left = 9000, right = -7000;
                std::memcpy(bytes.data() + block * 1024 + frame * 2, &left, 2);
                std::memcpy(bytes.data() + block * 1024 + 512 + frame * 2, &right, 2);
            }
        spu.dmaTransfer(1, true, bytes.data(), 1024);
        check(spu.dmaCompletionCycles(1, 1024) == 768 * 768, "long AutoDMA finishes filling its final half with 256 unplayed frames remaining");
        const auto before = render(spu, 16);
        check(before.front() > 8000 && before.back() < -6000, "long stream plays before partial stop");
        spu.write16(0x5b0, 0);
        check((spu.read16(0x744) & 0x480) == 0, "ADMAS STOP clears busy/completion state without completed-transfer signal");
        const auto after = render(spu, 512);
        check(std::all_of(after.begin(), after.end(), [](int16_t sample) { return sample == 0; }), "ADMAS STOP discards all 1008 queued frames instead of draining old music");
        check(spu.statistics().dmaWrites == 1, "stopped stream does not restart itself");
        spu.write16(0x5b0, 2);
        std::fill(bytes.begin(), bytes.end(), uint8_t{0});
        for (unsigned i = 0; i != 256; ++i)
        {
            const int16_t left = 1000, right = 2000;
            std::memcpy(bytes.data() + i * 2, &left, 2);
            std::memcpy(bytes.data() + 512 + i * 2, &right, 2);
        }
        spu.dmaTransfer(1, true, bytes.data(), 256);
        const auto restarted = render(spu, 16);
        check(restarted.front() > 990 && restarted.front() < 1010 && restarted.back() > 1990, "explicit restart plays only the newly submitted planar buffer");
        spu.cancelDma(1);
        const auto chcrStop = render(spu, 256);
        check(std::all_of(chcrStop.begin(), chcrStop.end(), [](int16_t sample) { return !sample; }), "CHCR-facing cancelDma also discards pending PCM");
        check((spu.read16(0x744) & 0x480) == 0 && spu.read16(0x5b0) == 2, "CHCR cancellation leaves the ADMAS register intact for a later explicit start");
        spu.completeDma(1);
        check((spu.read16(0x744) & 0x480) == 0, "stale completeDma cannot resurrect a cancelled transfer");
    }
    void reverbNetwork()
    {
        IopSpu2 spu;
        const auto setup = [&](bool enabled)
        {
            spu.reset(); route(spu, 1);
            spu.write16(0x59a, enabled ? 0xc080 : 0xc000);
            spu.write16(0x598, 0x30); // AutoDMA input to wet L/R only.
            spu.write16(0x794, 0x7fff); spu.write16(0x796, 0x7fff);
            spu.write16(0x78c, 0x7fff); spu.write16(0x78e, 0x7fff);
            spu.write16(0x79c, 0x7fff); // IIR.
            spu.write16(0x79e, 0x7fff); // Comb 1.
            spu.write16(0x7ac, 0x7fff); spu.write16(0x7ae, 0x7fff); // Input gain.
            addr(spu, 1, 0x2e0, 0x80000);
            spu.write16(0x73c, 8);
            addr(spu, 1, 0x2ec, 0); addr(spu, 1, 0x2f0, 1);
            addr(spu, 1, 0x2f4, 0); addr(spu, 1, 0x2f8, 1);
            addr(spu, 1, 0x30c, 30); addr(spu, 1, 0x310, 31);
            addr(spu, 1, 0x32c, 10); addr(spu, 1, 0x330, 11);
            addr(spu, 1, 0x334, 20); addr(spu, 1, 0x338, 21);
            addr(spu, 1, 0x2e4, 10); addr(spu, 1, 0x2e8, 20);
            spu.write16(0x5b0, 2);
            std::vector<uint8_t> bytes(1024);
            for (unsigned i = 0; i != 256; ++i)
            {
                const int16_t left = 1000, right = -2000;
                std::memcpy(bytes.data() + i * 2, &left, 2);
                std::memcpy(bytes.data() + 512 + i * 2, &right, 2);
            }
            spu.dmaTransfer(1, true, bytes.data(), 256);
        };
        setup(true);
        const auto effect = render(spu, 64);
        check(effect[40] > 990 && effect[40] < 1010 && effect[41] < -1980, "programmed SPU2 IIR/comb/APF network renders wet stereo output");
        addr(spu, 0, 0x1a8, 0x80000);
        check(spu.read16(0x1ac) > 990, "reverb writes actual shared SPU RAM work area");
        setup(false);
        const auto disabled = render(spu, 64);
        check(std::all_of(disabled.begin(), disabled.end(), [](int16_t value) { return !value; }), "reverb write enable controls effect memory updates");
    }
    void captureRam()
    {
        IopSpu2 spu;
        route(spu, 0); route(spu, 1);
        upload(spu, 0, 0x9000, adpcm());
        voice(spu, 1, 1, 0x9000);
        unsigned irqs = 0;
        spu.setIrqCallback([&] { ++irqs; });
        addr(spu, 0, 0x19c, 0xc10);
        spu.write16(0x19a, 0xc040);
        render(spu, 20);
        check(irqs == 1 && (spu.read16(0x7c2) & 4), "other core voice capture buffer write triggers IRQA during actual playback");
        addr(spu, 0, 0x1a8, 0xc10);
        check(spu.read16(0x1ac) > 3500, "voice 1 capture holds post-envelope raw PCM");
        addr(spu, 0, 0x1a8, 0x1810);
        check(spu.read16(0x1ac) > 3500, "core 1 dry mixer capture holds real mixed PCM");
        spu.write16(0x19a, 0xc000);
        addr(spu, 0, 0x19c, 0xc10);
        spu.write16(0x19a, 0xc040);
        render(spu, 512);
        check(irqs == 2, "512-frame capture ring wraps and can retrigger after IRQ acknowledgement");
    }
    void memoryIntegration()
    {
        IopMemory memory;
        constexpr uint32_t base = 0x1f900000;
        memory.write32(base + 0x180, 0x00ab1234);
        check(memory.read16(base + 0x180) == 0x1234 && memory.read16(base + 0x182) == 0xab, "32-bit MMIO decomposes into independent halfword registers");
        memory.write8(base + 0x180, 0x56);
        memory.write8(base + 0x181, 0x78);
        check(memory.read32(base + 0x180) == 0x00ab7856, "byte MMIO preserves other register bytes");
        check(memory.read16(base + 0x580) == 0, "two core MMIO banks remain independent");
        const std::array<uint8_t, 16> input{31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16};
        check(memory.writeRam(0x10000, input.data(), input.size()), "fixture IOP source is real owned RAM");
        memory.write16(base + 0x19a, 0x8020);
        memory.write16(base + 0x1a8, 5);
        memory.write16(base + 0x1aa, 0x6780);
        memory.write32(0x1f8010c0, 0x10000);
        memory.write32(0x1f8010c4, 0x00010004);
        memory.write32(0x1f8010c8, 0x01000001);
        const auto write = memory.takeDmaStart();
        check(write && write->irq == 0x24 && write->delayCycles == 64, "real DMA schedule retains core 0 interrupt identity");
        check(memory.read32(0x1f8010c8) & 0x01000000, "CHCR start remains set until completion");
        check((memory.read16(base + 0x344) & 0x480) == 0x400, "actual memory exposes DMA busy");
        memory.completeSoundDma(0x24);
        check(!(memory.read32(0x1f8010c8) & 0x01000000) && (memory.read16(base + 0x344) & 0x80), "actual completion clears CHCR start and signals SPU complete");
        memory.write16(base + 0x5a8, 5);
        memory.write16(base + 0x5aa, 0x6780);
        memory.write32(0x1f801500, 0x11000);
        memory.write32(0x1f801504, 0x00010004);
        memory.write32(0x1f801508, 0x01000000);
        const auto read = memory.takeDmaStart();
        check(read && read->irq == 0x28, "SPU read DMA schedules core 1 interrupt");
        std::array<uint8_t, 16> output{};
        check(memory.readRam(0x11000, output.data(), output.size()) && output == input, "DMA read copies shared SPU RAM back into actual IOP RAM");
        memory.completeSoundDma(0x28);
        check(memory.read32(0x1f8010c0) == 0x10010 && memory.read32(0x1f801500) == 0x11010, "DMA increments real MADR by byte count");
        unsigned samples = 0;
        memory.spu2().setPcmCallback([&](std::span<const int16_t> pcm) { samples += static_cast<unsigned>(pcm.size()); });
        memory.reset();
        memory.spu2().advance(256 * 768);
        check(samples == 512 && memory.read16(base + 0x19a) == 0 && memory.read16(base + 0x59a) == 0, "IOP reset clears sound while preserving host callback");
    }
    void memoryDmaStop()
    {
        IopMemory memory;
        constexpr uint32_t base = 0x1f900000;
        route(memory.spu2(), 0); route(memory.spu2(), 1);
        memory.write16(base + 0x198, 0xc0);
        memory.write16(base + 0x598, 0xc);
        memory.write16(base + 0x76c, 0x7fff); memory.write16(base + 0x76e, 0x7fff);
        memory.write16(base + 0x790, 0x7fff); memory.write16(base + 0x792, 0x7fff);
        memory.write16(base + 0x1b0, 1);
        std::vector<uint8_t> bytes(32768); // Actual KFIV 8192-dword stream size.
        for (unsigned block = 0; block != 32; ++block)
            for (unsigned frame = 0; frame != 256; ++frame)
            {
                const int16_t sample = 7000;
                std::memcpy(bytes.data() + block * 1024 + frame * 2, &sample, 2);
                std::memcpy(bytes.data() + block * 1024 + 512 + frame * 2, &sample, 2);
            }
        check(memory.writeRam(0x10000, bytes.data(), bytes.size()), "retail-sized music fixture occupies real IOP RAM");
        memory.write32(0x1f8010c0, 0x10000);
        memory.write32(0x1f8010c4, 0x02000010);
        memory.write32(0x1f8010c8, 0x01000001);
        const auto event = memory.takeDmaStart();
        check(event && event->irq == 0x24 && event->delayCycles == (8192ull - 256) * 768, "retail-sized AutoDMA IRQ retains a genuine 256-frame refill reserve");
        check(memory.soundDmaActive(0x24) && !memory.soundDmaActive(0x28), "pending sound event is active only on its DMA channel");
        const auto playing = render(memory.spu2(), 16);
        check(playing.front() > 6900 && playing.back() > 6900, "actual IopMemory stream plays before CHCR STOP");
        memory.write32(0x1f8010c8, 1);
        check(!memory.soundDmaActive(0x24), "CHCR falling START cancels event eligibility for emulator dispatch");
        memory.write16(base + 0x1b0, 0);
        const auto stopped = render(memory.spu2(), 9000);
        check(std::all_of(stopped.begin(), stopped.end(), [](int16_t sample) { return !sample; }), "actual CHCR/ADMAS stop discards music beyond the old event deadline");
        check(memory.spu2().statistics().dmaWrites == 1 && !memory.takeDmaStart(), "no stale normal-DMA restart appears after the stream stop");
        check((memory.read16(base + 0x344) & 0x480) == 0 && memory.read32(0x1f8010c0) == 0x18000,
              "cancelled DMA preserves its completed byte copy without signalling old completion");
    }
}
int main()
{
    clockAndReset(); dmaAndPio(); adpcmPlayback(); routingNoiseAndSweep(); autoDma(); autoDmaRefillReserve(); autoDmaStop(); loopAndPmon(); reverbNetwork(); captureRam(); memoryIntegration(); memoryDmaStop();
    std::printf("Native SPU2/IOP audio tests passed: %u assertions.\n", checks);
}
