#include "ps2x/iop/iop_subsystem.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace ps2x::iop;
namespace
{
    unsigned checks = 0, failures = 0;
    void check(bool passed, const char *expression, int line)
    {
        ++checks;
        if (!passed) { ++failures; std::cerr << "FAIL line " << line << ": " << expression << '\n'; }
    }
#define CHECK(expression) check((expression), #expression, __LINE__)

    class TestHost final : public IopHost
    {
    public:
        std::optional<uint64_t> now;
        std::vector<uint8_t> guest = std::vector<uint8_t>(0x10000);
        uint64_t pcmFrames = 0;
        unsigned pcmCalls = 0;
        bool malformedPcm = false;
        std::vector<std::string> errors;
        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if ((!destination && size) || address > guest.size() || size > guest.size() - address) return false;
            if (size) std::memcpy(destination, guest.data() + address, size);
            return true;
        }
        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if ((!source && size) || address > guest.size() || size > guest.size() - address) return false;
            if (size) std::memcpy(guest.data() + address, source, size);
            return true;
        }
        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address) return false;
            std::fill_n(guest.data() + address, size, uint8_t{0});
            return true;
        }
        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override { normalized = address; return address <= guest.size(); }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        void audioSamples(std::span<const int16_t> pcm) override
        {
            malformedPcm |= pcm.size() % 2 != 0;
            pcmFrames += pcm.size() / 2;
            ++pcmCalls;
        }
        std::optional<uint64_t> monotonicNanoseconds() const noexcept override { return now; }
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel level, std::string_view message) override
        {
            if (level == LogLevel::Error || level == LogLevel::Warning) errors.emplace_back(message);
        }
    };

#pragma pack(push, 1)
    struct ElfHeader
    {
        uint8_t ident[16];
        uint16_t type, machine;
        uint32_t version, entry, phoff, shoff, flags;
        uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
    };
    struct ProgramHeader
    {
        uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
    };
#pragma pack(pop)
    constexpr uint32_t Base = 0x10000, TimerCounter = Base + 0x620;
    constexpr uint32_t Sid = 0xC10CFADE;
    constexpr uint32_t PeriodCycles = 153612; // FSIOPSND's approximately 4167 us tick.

    void syntheticTimerIrx(TestHost &host, bool criticalHandler = false, bool resumeInterrupts = true, bool blockingEvent = false)
    {
        // Retail-free executable MIPS fixture: register a repeating hard timer
        // and an RPC handler containing a controlled-length real instruction loop.
        std::vector<uint8_t> segment(0x900, 0);
        auto words = [&](size_t offset, std::span<const uint32_t> code) {
            std::memcpy(segment.data() + offset, code.data(), code.size_bytes());
        };
        std::vector<uint32_t> entry{0x27bdffe0, 0xafbf001c};
        auto emit = [&](uint32_t word) { entry.push_back(word); };
        auto li = [&](unsigned reg, uint32_t value) {
            emit(0x3c000000 | (reg << 16) | (value >> 16));
            emit(0x34000000 | (reg << 21) | (reg << 16) | (value & 0xffff));
        };
        auto call = [&](uint32_t address) { emit(0x0c000000 | (address >> 2)); emit(0); };
        constexpr uint32_t EventImports = Base + 0x240;
        if (blockingEvent)
        {
            li(4, Base + 0x680); call(EventImports + 20); // Create an initially clear event.
            li(8, Base + 0x690); emit(0xad020000);
        }
        constexpr uint32_t TimerImports = Base + 0x300, RpcImports = Base + 0x380;
        constexpr uint32_t Alloc = TimerImports + 20, Setup = Alloc + 8, Handler = Setup + 8, Start = Handler + 8;
        li(4, 1); li(5, 32); li(6, 1); call(Alloc);
        emit(0x00408021); // s0 = timer id
        emit(0x02002021); li(5, 1); li(6, 0); li(7, 1); call(Setup);
        emit(0x02002021); li(5, PeriodCycles); li(6, Base + 0x400); li(7, TimerCounter); call(Handler);
        emit(0x02002021); call(Start);
        li(4, Base + 0x700); li(5, Sid); li(6, Base + 0x500); li(7, Base + 0x800);
        emit(0xafa00010); emit(0xafa00014); emit(0xafa00018); call(RpcImports + 20);
        emit(0x8fbf001c); emit(0x00001021); emit(0x27bd0020); emit(0x03e00008); emit(0);
        words(0, entry);
        const std::array<uint32_t, 15> timerImports{
            0x41e00000, 0, 0x101, 0x726d6974, 0x006e616d, // timrman
            0x03e00008, 0x24000004, 0x03e00008, 0x24000016,
            0x03e00008, 0x24000014, 0x03e00008, 0x24000017, 0, 0};
        words(0x300, timerImports);
        const std::array<uint32_t, 9> rpcImports{
            0x41e00000, 0, 0x101, 0x63666973, 0x0000646d,
            0x03e00008, 0x24000011, 0, 0};
        words(0x380, rpcImports);
        const std::array<uint32_t, 9> intrImports{
            0x41e00000, 0, 0x101, 0x72746e69, 0x006e616d, // intrman
            0x03e00008, 0x24000011, 0x03e00008, 0x24000012};
        words(0x3c0, intrImports);
        const std::array<uint32_t, 8> callback{
            0x8c880000, 0, 0x25080001, 0xac880000, // Respect the R3000A load delay.
            0x3c020000 | (PeriodCycles >> 16), 0x34420000 | (PeriodCycles & 0xffff),
            0x03e00008, 0};
        words(0x400, callback);
        const std::array<uint32_t, 7> rpcHandler{
            0x00804021, // t0 = requested loop count
            0x2508ffff, // decrement
            0x1d00fffe, // bgtz t0, decrement
            0, 0x00a01021, 0x03e00008, 0}; // return RPC buffer
        words(0x500, rpcHandler);
        if (blockingEvent)
        {
            const std::array<uint32_t, 15> imports{
                0x41e00000, 0, 0x101, 0x76656874, 0x00746e65, // thevent
                0x03e00008, 0x24000004, 0x03e00008, 0x24000007,
                0x03e00008, 0x2400000a, 0x03e00008, 0x2400000b, 0, 0};
            words(0x240, imports);
            const std::vector<uint32_t> signal{
                0x27bdffe0, 0xafbf001c,
                0x8c880000, 0, 0x25080001, 0xac880000,
                0x3c080001, 0x35080690, 0x8d040000, 0, 0x34050001,
                0x0c000000 | ((EventImports + 28) >> 2), 0,
                0x3c020000 | (PeriodCycles >> 16), 0x34420000 | (PeriodCycles & 0xffff),
                0x8fbf001c, 0, 0x27bd0020, 0x03e00008, 0};
            words(0x400, signal);
            const std::vector<uint32_t> wait{
                0x27bdffe0, 0xafbf001c,
                0x3c080001, 0x35080690, 0x8d040000, 0,
                0x34050001, 0x34060010, 0x34070000, // AND, clear after success.
                0x0c000000 | ((EventImports + 36) >> 2), 0,
                0x3c080001, 0x35080800, 0xad020000, // Return the actual wait result.
                0x01001021, 0x8fbf001c, 0, 0x27bd0020, 0x03e00008, 0};
            words(0x500, wait);
        }
        if (criticalHandler)
        {
            constexpr uint32_t Suspend = Base + 0x3c0 + 20, Resume = Suspend + 8;
            std::vector<uint32_t> critical{
                0x27bdffe0, 0xafbf001c, 0x00808021, // stack frame, save requested iterations
                0x3c040001, 0x34840630, 0x0c000000 | (Suspend >> 2), 0,
                0x02004021, 0x2508ffff, 0x1d00fffe, 0, // controlled-length loop
                0x3c080001, 0x35080620, 0x8d090000, 0, 0xad090004, // snapshot callback count before resume
            };
            if (resumeInterrupts)
            {
                const std::array<uint32_t, 7> resume{
                    0x3c080001, 0x35080630, 0x8d040000, 0,
                    0x0c000000 | (Resume >> 2), 0, 0};
                critical.insert(critical.end(), resume.begin(), resume.end());
            }
            const std::array<uint32_t, 6> finish{
                0x3c020001, 0x34420800, 0x8fbf001c, 0x27bd0020, 0x03e00008, 0};
            critical.insert(critical.end(), finish.begin(), finish.end());
            words(0x500, critical);
        }

        ElfHeader header{};
        std::memcpy(header.ident, "\x7f" "ELF\x01\x01\x01", 7);
        header.type = 2; header.machine = 8; header.version = 1; header.entry = Base;
        header.phoff = sizeof(header); header.ehsize = sizeof(header);
        header.phentsize = sizeof(ProgramHeader); header.phnum = 1;
        ProgramHeader program{1, 0x100, Base, Base, static_cast<uint32_t>(segment.size()), static_cast<uint32_t>(segment.size()), 7, 4};
        constexpr uint32_t GuestElf = 0x100;
        std::memcpy(host.guest.data() + GuestElf, &header, sizeof(header));
        std::memcpy(host.guest.data() + GuestElf + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + GuestElf + 0x100, segment.data(), segment.size());
    }
    uint32_t timerCount(const IopSubsystem &iop)
    {
        uint32_t value = UINT32_MAX;
        CHECK(iop.readMemory(TimerCounter, &value, sizeof(value)));
        return value;
    }
    void loadFixture(TestHost &host, IopSubsystem &iop, bool criticalHandler = false, bool resumeInterrupts = true)
    {
        CHECK(!iop.hasEmulatedRpcServer(Sid));
        syntheticTimerIrx(host, criticalHandler, resumeInterrupts);
        const auto loaded = iop.loadModuleBuffer(0x100);
        CHECK(loaded.handled && loaded.moduleId > 0 && loaded.startResult == 0);
        CHECK(iop.canBindRpc(Sid));
        CHECK(iop.hasEmulatedRpcServer(Sid));
        CHECK(!iop.hasEmulatedRpcServer(0x80000701));
        CHECK(host.errors.empty());
    }
    void deterministicDefault()
    {
        TestHost host;
        IopSubsystem iop(host);
        iop.runEeCycles(7);
        CHECK(iop.debugSnapshot().emulatorCycles == 0);
        iop.runEeCycles(1);
        CHECK(iop.debugSnapshot().emulatorCycles == 1);
        iop.runEeCycles(196608 * 8 - 8);
        CHECK(iop.debugSnapshot().emulatorCycles == 196608);
        CHECK(host.pcmFrames == 256 && host.pcmCalls == 1 && !host.malformedPcm);
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == 196608);
    }
    void absoluteHostClock()
    {
        TestHost host;
        host.now = 10'000'000'000;
        IopSubsystem iop(host);
        iop.runEeCycles(999'999'999); // EE dispatch volume does not control native audio time.
        CHECK(iop.debugSnapshot().emulatorCycles == 0);
        host.now = *host.now + 1'000'000'000;
        iop.runEeCycles(1);
        CHECK(iop.debugSnapshot().emulatorCycles == 36'864'000);
        CHECK(host.pcmFrames == (48000 / 256) * 256);
        const auto frames = host.pcmFrames;
        iop.runEeCycles(999'999'999);
        CHECK(iop.debugSnapshot().emulatorCycles == 36'864'000 && host.pcmFrames == frames);
        CHECK(!host.malformedPcm && host.errors.empty());
    }
    void fractionalClockAndReset()
    {
        TestHost host;
        host.now = 0;
        IopSubsystem iop(host);
        for (uint64_t step = 1; step <= 1000; ++step)
        {
            host.now = step * 1'000'000;
            iop.runEeCycles(step * 123);
        }
        CHECK(iop.debugSnapshot().emulatorCycles == 36'864'000);
        CHECK(host.pcmFrames == (48000 / 256) * 256);
        host.now = *host.now + 1;
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == 36'864'000);
        host.now = *host.now + 1000;
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == 36'864'036);
        const auto cycles = iop.debugSnapshot().emulatorCycles;
        host.now = 500; // Regression rebases without rewinding or a huge unsigned burst.
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == cycles);
        host.now = 1'000'500;
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == cycles + 36864);
        iop.reset();
        CHECK(iop.debugSnapshot().emulatorCycles == 0);
        const auto priorFrames = host.pcmFrames;
        iop.runEeCycles(999999);
        CHECK(iop.debugSnapshot().emulatorCycles == 0 && host.pcmFrames == priorFrames);
        host.now = *host.now + 1'000'000'000;
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == 36'864'000);
        CHECK(host.pcmFrames - priorFrames == (48000 / 256) * 256);
    }
    void timerCatchupAndDirectExecution()
    {
        TestHost host;
        host.now = 0;
        IopSubsystem iop(host);
        loadFixture(host, iop);
        const auto startupCycles = iop.debugSnapshot().emulatorCycles;
        CHECK(startupCycles > 0 && startupCycles < 1000);
        RpcRequest request{};
        request.sid = Sid; request.function = 1000;
        const auto rpc = iop.handleRpc(request);
        CHECK(rpc.handled);
        const auto directCycles = iop.debugSnapshot().emulatorCycles;
        CHECK(directCycles >= startupCycles + 3000);
        iop.runEeCycles(999999);
        CHECK(iop.debugSnapshot().emulatorCycles == directCycles);
        host.now = 1'000'000;
        iop.runEeCycles(999999);
        CHECK(iop.debugSnapshot().emulatorCycles == 36864);
        CHECK(timerCount(iop) == 0);
        host.now = 250'000'000; // A slow renderer's elapsed interval still executes all timer periods.
        iop.runEeCycles(0);
        CHECK(iop.debugSnapshot().emulatorCycles == 9'216'000);
        const auto count250 = timerCount(iop);
        if (count250 != 59) std::cerr << "250ms timer count=" << count250 << '\n';
        CHECK(count250 == 59);
        CHECK(host.pcmFrames == (12000 / 256) * 256);
        host.now = 300'000'000;
        iop.runEeCycles(0);
        const auto count300 = timerCount(iop);
        if (count300 != 71) std::cerr << "300ms timer count=" << count300 << '\n';
        CHECK(count300 == 71);
        CHECK(host.errors.empty());
    }
    void clockCapabilityTransitions()
    {
        TestHost host;
        IopSubsystem iop(host);
        iop.runEeCycles(800);
        CHECK(iop.debugSnapshot().emulatorCycles == 100);
        host.now = 5'000'000'000;
        iop.runEeCycles(800);
        CHECK(iop.debugSnapshot().emulatorCycles == 100);
        host.now = *host.now + 1'000'000;
        iop.runEeCycles(800);
        CHECK(iop.debugSnapshot().emulatorCycles == 36964);
        host.now.reset();
        iop.runEeCycles(800);
        CHECK(iop.debugSnapshot().emulatorCycles == 37064);
        host.now = 100'000'000'000;
        iop.runEeCycles(800);
        CHECK(iop.debugSnapshot().emulatorCycles == 37064);
    }
    void directCallTimers()
    {
        TestHost host;
        host.now = 0;
        IopSubsystem iop(host);
        loadFixture(host, iop);
        RpcRequest request{};
        request.sid = Sid; request.function = 400000;
        CHECK(iop.handleRpc(request).handled);
        CHECK(iop.debugSnapshot().emulatorCycles >= 1'200'000);
        const auto countDirect = timerCount(iop);
        if (countDirect != 7) std::cerr << "direct timer count=" << countDirect << '\n';
        CHECK(countDirect == 7);
        CHECK(host.errors.empty());
    }
    void interruptCriticalSections()
    {
        for (const bool resume : {true, false})
        {
            TestHost host;
            host.now = 0;
            IopSubsystem iop(host);
            loadFixture(host, iop, true, resume);
            RpcRequest request{};
            request.sid = Sid; request.function = 400000;
            CHECK(iop.handleRpc(request).handled);
            uint32_t during = UINT32_MAX, originalControl = 0;
            CHECK(iop.readMemory(TimerCounter + 4, &during, sizeof(during)));
            CHECK(iop.readMemory(TimerCounter + 16, &originalControl, sizeof(originalControl)));
            CHECK(during == 0 && originalControl == 1);
            host.now = 50'000'000;
            iop.runEeCycles(0);
            // A pending timer interrupt delivers after resume. Masked periods
            // consolidate into one pending interrupt rather than retroactive ticks.
            CHECK(timerCount(iop) == (resume ? 5u : 0u));
            CHECK(iop.debugSnapshot().emulatorCycles == 1'843'200);
            CHECK(host.errors.empty());
        }
    }
    void dmaCriticalSection()
    {
        TestHost host;
        IopSubsystem iop(host); // Default deterministic clock for the instruction-level ordering test.
        std::array<uint32_t, 4> source{0x12345678, 0x23456789, 0x3456789a, 0x456789ab};
        CHECK(iop.writeMemory(0x40000, source.data(), sizeof(source)));
        std::vector<uint32_t> entry{0x27bdffe0, 0xafbf001c};
        auto emit = [&](uint32_t word) { entry.push_back(word); };
        auto li = [&](unsigned reg, uint32_t value) {
            emit(0x3c000000 | (reg << 16) | (value >> 16));
            emit(0x34000000 | (reg << 21) | (reg << 16) | (value & 0xffff));
        };
        auto call = [&](uint32_t address) { emit(0x0c000000 | (address >> 2)); emit(0); };
        constexpr uint32_t Imports = Base + 0x300;
        li(4, 0x24); li(5, 1); li(6, Base + 0x400); li(7, TimerCounter); call(Imports + 20);
        li(4, 0x24); call(Imports + 28); // Enable DMA IRQ handler.
        li(4, 0x28); li(5, 1); li(6, Base + 0x500); li(7, TimerCounter + 32); call(Imports + 20);
        li(4, 0x28); call(Imports + 28);
        li(4, TimerCounter + 16); call(Imports + 36); // CpuSuspendIntr.
        li(24, 0x1f900000);
        li(8, 0xc000); emit(0xa708019a); // SPU core attribute.
        emit(0xa70001a8); emit(0xa70001aa); emit(0xa70001b0); // TSA 0, ordinary sound DMA.
        li(25, 0x1f801000);
        li(8, 0x40000); emit(0xaf2800c0); // MADR.
        li(8, 4); emit(0xaf2800c4); // Four dwords, normal DMA completion after >=64 cycles.
        li(8, 0x01000001); emit(0xaf2800c8); // START.
        li(8, 1024); emit(0x2508ffff); emit(0x1d00fffe); emit(0);
        li(8, TimerCounter); emit(0x8d090000); emit(0); emit(0xad090004); // Observe IRQ count inside critical section.
        li(8, 0x1f801000); emit(0x8d0900c8); emit(0); li(8, TimerCounter); emit(0xad09000c);
        emit(0x8d040010); emit(0); call(Imports + 44); // CpuResumeIntr(original).
        li(8, 1024); emit(0x2508ffff); emit(0x1d00fffe); emit(0);
        li(8, 0x1f801000); emit(0x8d0900c8); emit(0); li(8, TimerCounter); emit(0xad090008); // Observe completed CHCR.
        emit(0x8fbf001c); emit(0x27bd0020); emit(0x00001021); emit(0x03e00008); emit(0);
        CHECK(entry.size() * 4 < 0x300);
        std::vector<uint8_t> segment(0x800, 0);
        std::memcpy(segment.data(), entry.data(), entry.size() * 4);
        const std::array<uint32_t, 17> imports{
            0x41e00000, 0, 0x101, 0x72746e69, 0x006e616d,
            0x03e00008, 0x24000004, 0x03e00008, 0x24000006,
            0x03e00008, 0x24000011, 0x03e00008, 0x24000012,
            0x03e00008, 0x24000017, 0, 0};
        std::memcpy(segment.data() + 0x300, imports.data(), imports.size() * 4);
        const std::vector<uint32_t> callback{
            0x27bdffe0, 0xafbf001c, 0xafa40018,
            0x0c000000 | ((Imports + 52) >> 2), 0, // QueryIntrContext inside DMA handler.
            0x8fa40018, 0, 0xac820014,
            0x8c880000, 0, 0x25080001, 0xac880000,
            // Start another hardware DMA inside the first interrupt handler.
            // Its hardware completes, but its handler cannot preempt this one.
            0x3c181f90, 0x3408c000, 0xa708059a, 0xa70005a8, 0xa70005aa, 0xa70005b0,
            0x3c191f80, 0x37391500, 0x3c080004, 0xaf280000,
            0x34080004, 0xaf280004, 0x3c080100, 0x35080001, 0xaf280008,
            0x34080400, 0x2508ffff, 0x1d00fffe, 0,
            0x8f290008, 0, 0xac89001c, // CHCR after transfer completes inside interrupt.
            0x8c880020, 0, 0xac880018, // Other IRQ count while this handler is running.
            0x8fbf001c, 0, 0x27bd0020, 0x03e00008, 0};
        std::memcpy(segment.data() + 0x400, callback.data(), callback.size() * 4);
        const std::array<uint32_t, 6> secondCallback{0x8c880000, 0, 0x25080001, 0xac880000, 0x03e00008, 0};
        std::memcpy(segment.data() + 0x500, secondCallback.data(), secondCallback.size() * 4);
        ElfHeader header{};
        std::memcpy(header.ident, "\x7f" "ELF\x01\x01\x01", 7);
        header.type = 2; header.machine = 8; header.version = 1; header.entry = Base;
        header.phoff = sizeof(header); header.ehsize = sizeof(header); header.phentsize = sizeof(ProgramHeader); header.phnum = 1;
        ProgramHeader program{1, 0x100, Base, Base, static_cast<uint32_t>(segment.size()), static_cast<uint32_t>(segment.size()), 7, 4};
        std::memcpy(host.guest.data() + 0x100, &header, sizeof(header));
        std::memcpy(host.guest.data() + 0x100 + sizeof(header), &program, sizeof(program));
        std::memcpy(host.guest.data() + 0x200, segment.data(), segment.size());
        const auto loaded = iop.loadModuleBuffer(0x100);
        CHECK(loaded.handled && loaded.startResult == 0);
        CHECK(timerCount(iop) == 1);
        uint32_t during = UINT32_MAX, control = 0, chcr = UINT32_MAX;
        CHECK(iop.readMemory(TimerCounter + 4, &during, 4));
        CHECK(iop.readMemory(TimerCounter + 16, &control, 4));
        CHECK(iop.readMemory(TimerCounter + 8, &chcr, 4));
        CHECK(during == 0 && control == 1);
        CHECK((chcr & 0x01000000) == 0);
        uint32_t maskedChcr = UINT32_MAX, interruptContext = 0;
        CHECK(iop.readMemory(TimerCounter + 12, &maskedChcr, 4));
        CHECK(iop.readMemory(TimerCounter + 20, &interruptContext, 4));
        CHECK((maskedChcr & 0x01000000) == 0); // Hardware finishes before CPU interrupts resume.
        CHECK(interruptContext == 1);
        uint32_t nestedCount = UINT32_MAX, secondChcr = UINT32_MAX, secondCount = 0;
        CHECK(iop.readMemory(TimerCounter + 24, &nestedCount, 4));
        CHECK(iop.readMemory(TimerCounter + 28, &secondChcr, 4));
        CHECK(iop.readMemory(TimerCounter + 32, &secondCount, 4));
        CHECK(nestedCount == 0 && secondCount == 1);
        CHECK((secondChcr & 0x01000000) == 0);
        iop.runEeCycles(8000);
        CHECK(timerCount(iop) == 1); // No duplicate callback after normal completion.
        CHECK(host.errors.empty());
    }
    void synchronousRpcEventWait()
    {
        TestHost host;
        IopSubsystem iop(host);
        syntheticTimerIrx(host, false, true, true);
        const auto loaded = iop.loadModuleBuffer(0x100);
        CHECK(loaded.handled && loaded.moduleId > 0 && loaded.startResult == 0);
        RpcRequest request{};
        request.sid = Sid;
        request.receive = {0x2000, 4};
        for (unsigned expected = 1; expected <= 2; ++expected)
        {
            const auto before = iop.debugSnapshot().emulatorCycles;
            CHECK(iop.handleRpc(request).signalCompletion);
            uint32_t result = UINT32_MAX;
            std::memcpy(&result, host.guest.data() + 0x2000, 4);
            CHECK(result == 0);
            CHECK(timerCount(iop) == expected);
            CHECK(iop.debugSnapshot().emulatorCycles - before > PeriodCycles - 1000);
        }
        CHECK(host.errors.empty());
    }
}
int main()
{
    deterministicDefault();
    absoluteHostClock();
    fractionalClockAndReset();
    timerCatchupAndDirectExecution();
    clockCapabilityTransitions();
    directCallTimers();
    interruptCriticalSections();
    dmaCriticalSection();
    synchronousRpcEventWait();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
