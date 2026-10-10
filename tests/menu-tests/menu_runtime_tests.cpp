#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "runtime/kfiv_runtime.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace
{
    constexpr uint32_t kFree = 0x00228690u;
    constexpr uint32_t kContinuation = 0x002286ECu;
    constexpr uint32_t kRetailInit = 0x004382FCu;
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kCrc = 0xBDA82D37u;
    constexpr std::array<std::array<uint32_t, 2>, 8> kInstructions = {{
        {kFree, 0x3C030044u}, {0x00228698u, 0x8C6282FCu},
        {0x0022869Cu, 0x0080282Du}, {0x002286A4u, 0x04410003u},
        {0x002286B0u, 0x0000102Du}, {0x002286D0u, 0x24050002u},
        {0x002286E4u, 0x0C089240u}, {0x00228700u, 0x03E00008u},
    }};
    unsigned checks = 0;
    unsigned failures = 0;
    unsigned nativeFrees = 0;
    uint32_t lastFreeArgument = 0;
    uint8_t *lastRdram = nullptr;

    void check(bool condition, std::string_view reason)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::cerr << "FAIL: " << reason << '\n';
        }
    }

    // Control condition representing the former unmapped SDK free path: its
    // retail initialization word is negative, so it reports success without
    // releasing the native allocation. No retail code or data is included.
    void unmappedFree(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        check(runtime->memory().read32(kRetailInit) == 0xFFFFFFFFu,
              "control no longer represents uninitialized retail RPC client");
        setReturnS32(ctx, 0);
        ctx->pc = getRegU32(ctx, 31);
    }

    void continuation(uint8_t *, R5900Context *, PS2Runtime *) {}

    void prepare(PS2Runtime &runtime)
    {
        for (const auto &item : kInstructions)
            runtime.ee.words[item[0]] = item[1];
        runtime.ee.words[kRetailInit] = 0xFFFFFFFFu;
        // Original gameplay loader guards remain untouched throughout tests.
        runtime.ee.words[0x0044685Cu] = 0u;
        runtime.ee.words[0x0038A134u] = 1u;
        runtime.functions[kFree] = &unmappedFree;
        runtime.functions[kContinuation] = &continuation;
    }

    void apply(PS2Runtime &runtime, const std::string &name = "SLUS_203.18",
               uint32_t entry = kEntry, uint32_t crc = kCrc, bool valid = true)
    {
        ps2_game_overrides::applyMatching(runtime, name, entry, crc, valid);
    }

    int32_t callFree(PS2Runtime &runtime, uint32_t address)
    {
        R5900Context ctx{};
        ctx.pc = kFree;
        ctx.registers[4] = address;
        ctx.registers[29] = 0x01FFE000u;
        ctx.registers[31] = 0x001CD74Cu;
        runtime.lookupFunction(kFree)(nullptr, &ctx, &runtime);
        return static_cast<int32_t>(ctx.registers[2]);
    }

    void metadataAndInstructionGuards()
    {
        struct Metadata
        {
            const char *path;
            uint32_t entry;
            uint32_t crc;
            bool valid;
        };
        constexpr Metadata rejected[] = {
            {"SLUS_203.19", kEntry, kCrc, true},
            {"SLUS_203.18", kEntry + 4u, kCrc, true},
            {"SLUS_203.18", kEntry, kCrc ^ 1u, true},
            {"SLUS_203.18", kEntry, kCrc, false},
        };
        for (const auto &item : rejected)
        {
            PS2Runtime runtime;
            prepare(runtime);
            const auto words = runtime.ee.words;
            apply(runtime, item.path, item.entry, item.crc, item.valid);
            check(runtime.lookupFunction(kFree) == &unmappedFree,
                  "wrong ELF metadata installed heap free binding");
            check(runtime.replacementAttempts == 0u, "metadata rejection attempted replacement");
            check(runtime.ee.words == words, "metadata rejection changed game RAM");
        }
        for (const auto &item : kInstructions)
        {
            PS2Runtime runtime;
            prepare(runtime);
            runtime.ee.words[item[0]] ^= 1u;
            const auto words = runtime.ee.words;
            apply(runtime);
            check(runtime.lookupFunction(kFree) == &unmappedFree,
                  "instruction mismatch installed heap free binding");
            check(runtime.replacementAttempts == 0u, "guard rejection attempted replacement");
            check(runtime.ee.words == words, "guard rejection changed game RAM");
        }
        PS2Runtime missing;
        prepare(missing);
        missing.functions.erase(kFree);
        apply(missing);
        check(!missing.hasFunction(kFree), "missing compiled SDK entry was created");
        check(missing.replacementAttempts == 0u, "missing entry attempted replacement");

        PS2Runtime inaccessible;
        prepare(inaccessible);
        inaccessible.ee.failReads = true;
        apply(inaccessible);
        check(inaccessible.lookupFunction(kFree) == &unmappedFree,
              "unavailable EE memory installed a binding");

        PS2Runtime failedReplace;
        prepare(failedReplace);
        failedReplace.failReplacement = true;
        apply(failedReplace);
        check(failedReplace.lookupFunction(kFree) == &unmappedFree,
              "failed replacement changed original entry");
        check(failedReplace.lookupFunction(kContinuation) == &continuation,
              "failed replacement changed retail continuation");
    }

    void entryAndContext()
    {
        PS2Runtime runtime;
        prepare(runtime);
        const auto words = runtime.ee.words;
        for (int i = 0; i < 5; ++i)
            ps2_kfiv_runtime::registerOverrides();
        apply(runtime, "C:/disc/slus_203.18");
        const auto installed = runtime.lookupFunction(kFree);
        check(installed != &unmappedFree && installed != nullptr, "valid SDK entry was not bound");
        check(runtime.replacementAttempts == 1u, "descriptor registered more than once");
        check(runtime.lookupFunction(kContinuation) == &continuation,
              "entry binding changed retail continuation");
        check(runtime.ee.words == words, "binding changed instructions, init or loader guards");
        apply(runtime);
        check(runtime.lookupFunction(kFree) == installed, "repeat application changed handler identity");
        check(runtime.replacementAttempts == 2u, "repeat application added duplicate descriptor");

        const uint32_t address = runtime.allocateIopMemory(0x5000u);
        check(address != 0u, "setup allocation failed");
        R5900Context ctx;
        for (size_t i = 0; i < ctx.registers.size(); ++i)
            ctx.registers[i] = 0xA0000000u + static_cast<uint32_t>(i);
        ctx.registers[4] = address;
        ctx.registers[29] = 0x01FFE000u;
        ctx.registers[31] = 0x001CD74Cu;
        ctx.pc = kFree;
        const auto before = ctx.registers;
        uint8_t rdramMarker = 0x5Au;
        installed(&rdramMarker, &ctx, &runtime);
        check(ctx.pc == before[31], "native SDK binding did not return to RA");
        check(ctx.registers[2] == 0u, "successful native free result not propagated");
        for (size_t i = 0; i < before.size(); ++i)
            if (i != 2u)
                check(ctx.registers[i] == before[i], "SDK free changed preserved register/SP/RA/arg");
        check(lastFreeArgument == address && lastRdram == &rdramMarker,
              "binding did not forward the native stub's arguments");
        check(!runtime.iop.allocationContaining(address).has_value(), "native stub left allocation live");
        check(runtime.ee.words == words, "native free changed retail game RAM");
        check(callFree(runtime, address) == -1, "double-free failure result not propagated");
        check(callFree(runtime, 0u) == -1, "null-free failure result not propagated");
    }

    void reclaimAndReuse()
    {
        PS2Runtime runtime;
        prepare(runtime);
        const uint32_t full = runtime.iop.maxFreeMemory();
        unsigned unreclaimed = 0;
        for (; unreclaimed < 200u; ++unreclaimed)
        {
            const uint32_t address = runtime.allocateIopMemory(0x5000u);
            if (!address)
                break;
            check(callFree(runtime, address) == 0, "control free should report false success");
        }
        check(unreclaimed > 0u && unreclaimed < 200u,
              "control did not reproduce exhaustion from unmapped frees");
        check(runtime.iop.maxFreeMemory() < 0x5000u, "control heap unexpectedly reclaimed its blocks");
        std::cout << "Control: unmapped free exhausted the real IOP heap after "
                  << unreclaimed << " nominal frees.\n";

        runtime.iop.reset();
        apply(runtime);
        const auto words = runtime.ee.words;
        uint32_t firstAddress = 0u;
        uint32_t secondAddress = 0u;
        for (unsigned cycle = 0; cycle < 300u; ++cycle)
        {
            const uint32_t first = runtime.allocateIopMemory(0x5000u);
            const uint32_t second = runtime.allocateIopMemory(0x5000u);
            check(first != 0u && second != 0u && first != second,
                  "two-bank resource allocation failed after previous cleanup");
            if (cycle == 0u)
            {
                firstAddress = first;
                secondAddress = second;
            }
            check(first == firstAddress && second == secondAddress,
                  "freed native resource buffers were not reused");
            check(callFree(runtime, first) == 0 && callFree(runtime, second) == 0,
                  "bound native free failed to release resource buffers");
            check(runtime.iop.maxFreeMemory() == full, "free cycle leaked native IOP memory");
            check(runtime.ee.words == words && runtime.ee.words[kRetailInit] == 0xFFFFFFFFu,
                  "cleanup changed retail init or gameplay loader guards");
        }
        std::cout << "Bound SDK: 300 two-buffer cycles reclaimed/reused memory; retail init remains FFFFFFFF.\n";
    }
}

// The actual game override registry is compiled. Unrelated input registration
// stays inert; this test checks the heap binding independently of device APIs.
namespace ps2_kfiv_input
{
    void registerOverrides() {}
}

namespace ps2_stubs
{
    // Minimal native SDK boundary: same Free stub operation as SIF.cpp, backed
    // by the actual iop_memory.cpp allocator rather than a boolean mock.
    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++nativeFrees;
        lastRdram = rdram;
        lastFreeArgument = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(lastFreeArgument) ? 0 : -1);
    }
}

int main()
{
    metadataAndInstructionGuards();
    entryAndContext();
    reclaimAndReuse();
    check(nativeFrees == 603u, "unexpected native free dispatch count");
    if (failures)
    {
        std::cerr << "FAILED: " << failures << '/' << checks << " assertions\n";
        return 1;
    }
    std::cout << "PASS: " << checks << " assertions; metadata/instruction guards, preserved context, real IOP reclamation\n";
    return 0;
}
