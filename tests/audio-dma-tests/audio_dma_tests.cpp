#include "ps2x/iop/iop_subsystem.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace ps2x::iop;
namespace
{
    unsigned checks = 0;
    void check(bool condition, const char *message)
    {
        ++checks;
        if (!condition) { std::fprintf(stderr, "FAIL [%u]: %s\n", checks, message); std::exit(1); }
    }
    class Host final : public IopHost
    {
    public:
        std::vector<uint8_t> guest = std::vector<uint8_t>(0x10000);
        std::vector<int16_t> pcm;
        std::vector<std::string> logs;
        bool readGuest(uint32_t a, void *p, size_t n) const override
        { if (a > guest.size() || n > guest.size() - a) return false; std::memcpy(p, guest.data() + a, n); return true; }
        bool writeGuest(uint32_t a, const void *p, size_t n) override
        { if (a > guest.size() || n > guest.size() - a) return false; std::memcpy(guest.data() + a, p, n); return true; }
        bool zeroGuest(uint32_t a, size_t n) override
        { if (a > guest.size() || n > guest.size() - a) return false; std::memset(guest.data() + a, 0, n); return true; }
        bool normalizeGuestAddress(uint32_t a, uint32_t &normal) const override { normal = a; return a <= guest.size(); }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        void audioSamples(std::span<const int16_t> samples) override { pcm.insert(pcm.end(), samples.begin(), samples.end()); }
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view) override { return 0; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t, uint64_t, void *, size_t, size_t &) override { return false; }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view message) override { logs.emplace_back(message); }
        // monotonicNanoseconds() deliberately inherits nullopt: deterministic EE/8.
    };
#pragma pack(push, 1)
    struct ElfHeader
    {
        uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
        uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
    };
    struct ProgramHeader
    {
        uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
    };
#pragma pack(pop)
    static_assert(sizeof(ElfHeader) == 52 && sizeof(ProgramHeader) == 32);
    constexpr uint32_t FixtureBase = 0x10000;
    constexpr uint32_t Handler = FixtureBase + 0x400;
    constexpr uint32_t Counter = FixtureBase + 0x700;
    constexpr uint32_t Restart = Counter + 4;
    constexpr uint32_t Stage = Counter + 8;
    constexpr uint32_t Probe = Counter + 16;
    constexpr uint32_t MusicBuffer = 0x40000;
    constexpr uint32_t SentinelWord = 0x50000;
    constexpr uint16_t Sentinel = 0x55aa;
    struct Code
    {
        std::vector<uint32_t> words;
        void emit(uint32_t word) { words.push_back(word); }
        void li(unsigned reg, uint32_t value)
        { emit(0x3c000000 | (reg << 16) | (value >> 16)); emit(0x34000000 | (reg << 21) | (reg << 16) | (value & 0xffff)); }
        void sw(unsigned rt, unsigned base, uint16_t offset) { emit(0xac000000 | (base << 21) | (rt << 16) | offset); }
        void sh(unsigned rt, unsigned base, uint16_t offset) { emit(0xa4000000 | (base << 21) | (rt << 16) | offset); }
        void lw(unsigned rt, unsigned base, uint16_t offset) { emit(0x8c000000 | (base << 21) | (rt << 16) | offset); emit(0); }
        void call(uint32_t address) { emit(0x0c000000 | (address >> 2)); emit(0); }
        void ret() { emit(0x03e00008); emit(0x24020000); }
        void writeHalf(uint16_t offset, uint16_t value) { li(8, value); sh(8, 24, offset); }
        void startMusic()
        {
            li(25, 0x1f801000);
            li(8, MusicBuffer); sw(8, 25, 0xc0);
            li(8, 0x02000010); sw(8, 25, 0xc4); // 512 blocks x 16 dwords = retail 8192.
            li(8, 0x01000001); sw(8, 25, 0xc8);
        }
    };
    void image(Host &host, uint32_t load, const std::vector<uint8_t> &segment)
    {
        ElfHeader elf{};
        elf.ident[0] = 0x7f; elf.ident[1] = 'E'; elf.ident[2] = 'L'; elf.ident[3] = 'F';
        elf.ident[4] = 1; elf.ident[5] = 1; elf.ident[6] = 1;
        elf.type = 2; elf.machine = 8; elf.version = 1; elf.entry = load;
        elf.phoff = sizeof(ElfHeader); elf.ehsize = sizeof(ElfHeader); elf.phentsize = sizeof(ProgramHeader); elf.phnum = 1;
        ProgramHeader ph{1, 0x100, load, load, static_cast<uint32_t>(segment.size()), static_cast<uint32_t>(segment.size()), 7, 4};
        std::fill(host.guest.begin(), host.guest.end(), uint8_t{0});
        std::memcpy(host.guest.data() + 0x100, &elf, sizeof(elf));
        std::memcpy(host.guest.data() + 0x100 + sizeof(elf), &ph, sizeof(ph));
        std::memcpy(host.guest.data() + 0x200, segment.data(), segment.size());
    }
    void load(Host &host, IopSubsystem &iop, uint32_t base, const std::vector<uint8_t> &segment)
    {
        image(host, base, segment);
        const auto result = iop.loadModuleBuffer(0x100);
        if (!result.handled || result.startResult != 0)
            for (const auto &line : host.logs) std::fprintf(stderr, "%s\n", line.c_str());
        check(result.handled && result.moduleId > 0 && result.startResult == 0, "synthetic MIPS IRX entry returns successfully");
    }
    std::vector<uint8_t> codeSegment(const Code &code)
    {
        std::vector<uint8_t> bytes(code.words.size() * 4);
        std::memcpy(bytes.data(), code.words.data(), bytes.size());
        return bytes;
    }
    uint32_t readWord(IopSubsystem &iop, uint32_t address)
    {
        uint32_t result = 0;
        check(iop.readMemory(address, &result, sizeof(result)), "guest fixture result is readable from actual IOP RAM");
        return result;
    }
    void fixture(Host &host, IopSubsystem &iop)
    {
        Code entry;
        entry.emit(0x27bdffe0); entry.sw(31, 29, 28);
        entry.li(4, 0x24); entry.li(5, 1); entry.li(6, Handler); entry.li(7, Counter);
        entry.call(FixtureBase + 0x300 + 20); // intrman RegisterIntrHandler 4.
        entry.li(4, 0x24); entry.call(FixtureBase + 0x300 + 28); // EnableIntr 6.
        entry.li(24, 0x1f900000);
        entry.writeHalf(0x19a, 0xc000); entry.writeHalf(0x59a, 0xc000);
        entry.writeHalf(0x760, 0x3fff); entry.writeHalf(0x762, 0x3fff);
        entry.writeHalf(0x788, 0x3fff); entry.writeHalf(0x78a, 0x3fff);
        entry.writeHalf(0x198, 0xc0); entry.writeHalf(0x598, 0xc);
        entry.writeHalf(0x76c, 0x7fff); entry.writeHalf(0x76e, 0x7fff);
        entry.writeHalf(0x790, 0x7fff); entry.writeHalf(0x792, 0x7fff);
        entry.writeHalf(0x1a8, SentinelWord >> 16); entry.writeHalf(0x1aa, SentinelWord & 0xffff);
        entry.writeHalf(0x1ac, Sentinel);
        entry.writeHalf(0x1a8, 0); entry.writeHalf(0x1aa, 0);
        entry.writeHalf(0x1b0, 1);
        entry.li(9, Restart); entry.li(8, 1); entry.sw(8, 9, 0);
        entry.startMusic();
        entry.li(8, 4096);
        entry.emit(0x2508ffff); // 4096 x (decrement, branch, delay nop) = 16 frames.
        entry.emit(0x1500fffe); entry.emit(0);
        entry.li(25, 0x1f801000); entry.li(8, 1); entry.sw(8, 25, 0xc8); // CHCR START falls.
        entry.writeHalf(0x1b0, 0); // libsd STOP also disables ADMAS.
        entry.li(9, Stage); entry.li(8, 1); entry.sw(8, 9, 0);
        entry.lw(31, 29, 28); entry.emit(0x27bd0020); entry.ret();
        check(entry.words.size() * 4 < 0x300, "fixture entry does not overlap import table");

        Code callback;
        callback.li(9, Counter); callback.lw(8, 9, 0);
        callback.emit(0x25080001); callback.sw(8, 9, 0);
        callback.lw(8, 9, 4);
        const size_t branch = callback.words.size(); callback.emit(0); callback.emit(0);
        // Model real LIBSD work between DMA IRQ and the next CHCR start.
        // Four sample periods must consume the already-filled MEMIN reserve.
        callback.li(10, 1024);
        callback.emit(0x254affff); callback.emit(0x1540fffe); callback.emit(0);
        callback.startMusic(); // Models libsd looping transfer handler after a completion.
        const size_t end = callback.words.size();
        callback.words[branch] = 0x11000000 | static_cast<uint16_t>(end - branch - 1);
        callback.ret();
        std::vector<uint8_t> segment(0x800);
        std::memcpy(segment.data(), entry.words.data(), entry.words.size() * 4);
        const uint32_t imports[] = {0x41e00000, 0, 0x101, 0x72746e69, 0x006e616d,
            0x03e00008, 0x24000004, 0x03e00008, 0x24000006, 0, 0};
        std::memcpy(segment.data() + 0x300, imports, sizeof(imports));
        std::memcpy(segment.data() + 0x400, callback.words.data(), callback.words.size() * 4);
        load(host, iop, FixtureBase, segment);
    }
    uint32_t probe(Host &host, IopSubsystem &iop, uint32_t base)
    {
        Code code;
        code.li(24, 0x1f900000);
        code.writeHalf(0x1a8, SentinelWord >> 16); code.writeHalf(0x1aa, SentinelWord & 0xffff);
        code.emit(0x970801ac); code.emit(0); // lhu t0,DATA(t8), load delay.
        code.li(9, Probe); code.sw(8, 9, 0);
        code.li(25, 0x1f801000); code.lw(8, 25, 0xc8); code.sw(8, 9, 4);
        code.ret(); load(host, iop, base, codeSegment(code));
        return readWord(iop, Probe);
    }
}
int main(int argc, char **argv)
{
    const bool control = argc == 2 && std::string_view(argv[1]) == "--expect-leak";
    Host host;
    IopSubsystem iop(host);
    check(!host.monotonicNanoseconds(), "fixture uses deterministic EE/8 clock");
    std::vector<uint8_t> music(32768);
    for (unsigned block = 0; block != 32; ++block)
        for (unsigned frame = 0; frame != 256; ++frame)
        {
            const int16_t left = 7000, right = -5000;
            std::memcpy(music.data() + block * 1024 + frame * 2, &left, 2);
            std::memcpy(music.data() + block * 1024 + 512 + frame * 2, &right, 2);
        }
    check(iop.writeMemory(MusicBuffer, music.data(), music.size()), "music fixture exists in physical IOP RAM");
    fixture(host, iop);
    check(readWord(iop, Stage) == 1 && readWord(iop, Counter) == 0, "entry stops after sixteen frames before old DMA completion");
    iop.runEeCycles(8ull * 768 * 12000); // Cross old 8192-frame deadline plus wrap-inducing stale IRQ loop.
    const uint32_t callbacks = readWord(iop, Counter);
    const uint32_t sentinel = probe(host, iop, 0x20000);
    if (control)
    {
        check(callbacks > 64, "original dispatch control invokes the guest DMA handler repeatedly after STOP");
        check(sentinel != Sentinel, "original guest callback loop overwrites unrelated SPU sample RAM");
        std::printf("Original stale-IRQ control reproduced: %u callbacks, sentinel=%04X, %u assertions.\n", callbacks, sentinel, checks);
        return 0;
    }
    check(callbacks == 0, "no cancelled old IRQ reaches the actual guest interrupt handler");
    check(sentinel == Sentinel, "no stale handler restarts normal DMA or overwrites SPU sample RAM");
    check(!(readWord(iop, Probe + 4) & 0x01000000), "cancelled guest DMA CHCR remains stopped");
    check(host.pcm.size() >= (12000 / 256) * 256 * 2, "sound continues generating full PCM chunks at the deterministic IOP clock");
    check(std::any_of(host.pcm.begin(), host.pcm.begin() + 32, [](int16_t value) { return value != 0; }), "actual guest setup played music before STOP");
    check(std::all_of(host.pcm.begin() + 64, host.pcm.end(), [](int16_t value) { return value == 0; }), "actual stopped stream produces silence beyond its completion deadline");
    uint32_t zero = 0;
    check(iop.writeMemory(Restart, &zero, 4), "disable optional looping for the next completed transfer");
    Code start;
    start.li(24, 0x1f900000); start.writeHalf(0x1a8, 0); start.writeHalf(0x1aa, 0); start.writeHalf(0x1b0, 1);
    start.startMusic(); start.ret();
    load(host, iop, 0x21000, codeSegment(start));
    const size_t pcmBefore = host.pcm.size();
    iop.runEeCycles(8ull * 768 * 8300);
    check(readWord(iop, Counter) == 1, "new explicit transfer completes and reaches the guest IRQ handler exactly once");
    check(std::any_of(host.pcm.begin() + pcmBefore, host.pcm.end(), [](int16_t value) { return value != 0; }), "new explicit transfer resumes actual music PCM");
    check(probe(host, iop, 0x22000) == Sentinel, "new AutoDMA completion preserves unrelated sample RAM");
    check(!(readWord(iop, Probe + 4) & 0x01000000), "new completed transfer clears guest CHCR START");
    uint32_t one = 1;
    check(iop.writeMemory(Restart, &one, 4), "enable genuine guest AutoDMA LOOP for multiple buffers");
    load(host, iop, 0x23000, codeSegment(start));
    const size_t loopPcmBefore = host.pcm.size();
    iop.runEeCycles(8ull * 768 * 48000);
    check(readWord(iop, Counter) == 6, "five genuine looping buffer completions reach the guest handler");
    const auto loopBegin = host.pcm.begin() + loopPcmBefore + 512;
    const size_t silentSamples = static_cast<size_t>(std::count(loopBegin, host.pcm.end(), int16_t{0}));
    check(silentSamples == 0, "MEMIN reserve covers genuine guest IRQ-handler work with zero silent LOOP frames");
    check(probe(host, iop, 0x24000) == Sentinel, "genuine AutoDMA LOOP never overwrites unrelated ADPCM RAM");
    check(readWord(iop, Probe + 4) & 0x01000000, "next looping transfer remains active after normal callbacks");
    Code stop;
    stop.li(25, 0x1f801000); stop.li(8, 1); stop.sw(8, 25, 0xc8);
    stop.li(24, 0x1f900000); stop.writeHalf(0x1b0, 0); stop.ret();
    load(host, iop, 0x25000, codeSegment(stop));
    iop.runEeCycles(8ull * 768 * 9000);
    check(readWord(iop, Counter) == 6, "STOP after genuine LOOP completions still rejects the next old IRQ");
    std::printf("Genuine AutoDMA LOOP: five completions, %zu silent sample values across one second.\n", silentSamples);
    std::printf("Actual guest DMA cancellation/restart passed: %u assertions.\n", checks);
}
