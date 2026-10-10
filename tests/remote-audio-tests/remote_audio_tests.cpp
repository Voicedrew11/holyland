#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"

#include <array>
#include <iostream>
#include <string_view>

namespace
{
    constexpr uint32_t kInit = 0x237E08u, kRemote = 0x237F48u;
    constexpr uint32_t kSifInit = 0x2240C0u, kBind = 0x224730u, kCall = 0x224900u;
    constexpr uint32_t kPacket = 0x1E55480u, kClient = 0x1E554C0u, kCallerStack = 0x1FFE000u;
    constexpr std::array<std::array<uint32_t, 2>, 17> kRemoteInstructions = {{
        {kInit,0x27BDFFC0u}, {0x237E20u,0x261254C0u}, {0x237E30u,0x3C058000u},
        {0x237E3Cu,0x0C0891CCu}, {0x237E94u,0x8E420024u}, {kRemote,0x27BDFF30u},
        {0x237F58u,0xFFA50098u}, {0x237F60u,0xFFA600A0u}, {0x237F90u,0xAE025480u},
        {0x237FA4u,0xACC20000u}, {0x23811Cu,0x24080040u}, {0x238124u,0x240A0010u},
        {0x238128u,0x0C089240u}, {0x238130u,0x8E115480u},
        {kSifInit,0x27BDFFC0u}, {kBind,0x27BDFF90u}, {kCall,0x27BDFF40u}
    }};
    constexpr std::array<std::array<uint32_t, 2>, 8> kHeapInstructions = {{
        {0x228690u,0x3C030044u}, {0x228698u,0x8C6282FCu}, {0x22869Cu,0x0080282Du},
        {0x2286A4u,0x04410003u}, {0x2286B0u,0x0000102Du}, {0x2286D0u,0x24050002u},
        {0x2286E4u,0x0C089240u}, {0x228700u,0x03E00008u}
    }};
    unsigned checks = 0, failures = 0;
    void check(bool condition, std::string_view reason)
    {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << reason << '\n'; }
    }
    struct SifFixture
    {
        unsigned inits = 0, binds = 0, calls = 0;
        bool initFailure = false, bindFailure = false, emptyServer = false, callFailure = false;
        int32_t response = 0x1234;
        std::array<uint32_t, 32> registers{};
        std::array<uint32_t, 16> packet{};
        uint32_t endParameter = 0;
    } sif;

    void original(uint8_t *, R5900Context *ctx, PS2Runtime *) { setReturnS32(ctx, -999); }
    void sifInit(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++sif.inits;
        check(getRegU32(ctx, 4) == 0u, "SIF init mode differs from retail zero");
        setReturnS32(ctx, sif.initFailure ? -1 : 0);
    }
    void sifBind(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++sif.binds;
        check(getRegU32(ctx, 4) == kClient, "bind uses wrong retail client");
        check(getRegU32(ctx, 5) == 0x80000701u, "bind uses wrong SDRDRV SID");
        check(getRegU32(ctx, 6) == 0u, "bind is not blocking");
        runtime->ee.write32(kClient + 0x24u, sif.emptyServer ? 0 : 0x2340u);
        setReturnS32(ctx, sif.bindFailure ? -1 : 0);
    }
    void sifCall(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++sif.calls;
        sif.registers = ctx->registers;
        for (unsigned i = 0; i < 16; ++i) sif.packet[i] = runtime->ee.read32(kPacket + i * 4u);
        sif.endParameter = runtime->ee.read32(getRegU32(ctx, 29));
        check(getRegU32(ctx, 29) != kCallerStack, "SIF call reused live caller stack");
        check(runtime->guestAllocations.contains(getRegU32(ctx, 29)), "SIF scratch stack not owned");
        if (!sif.callFailure && getRegU32(ctx, 9) == kPacket)
            runtime->ee.write32(kPacket, static_cast<uint32_t>(sif.response));
        setReturnS32(ctx, sif.callFailure ? -42 : 0);
    }
    void prepare(PS2Runtime &runtime)
    {
        sif = {};
        for (auto item : kRemoteInstructions) runtime.ee.words[item[0]] = item[1];
        for (auto item : kHeapInstructions) runtime.ee.words[item[0]] = item[1];
        runtime.functions[kInit] = &original;
        runtime.functions[kRemote] = &original;
        runtime.functions[kSifInit] = &sifInit;
        runtime.functions[kBind] = &sifBind;
        runtime.functions[kCall] = &sifCall;
        runtime.functions[0x228690u] = &original;
        runtime.physicalRemoteServer = true;
        // Deliberately plausible nonzero caller-stack values: legitimate zero
        // arguments must never be replaced by stale outgoing argument words.
        for (unsigned offset = 0; offset < 64; offset += 4)
            runtime.ee.words[kCallerStack + offset] = 0x500000u + offset;
    }
    void apply(PS2Runtime &runtime, uint32_t crc = 0xBDA82D37u)
    {
        ps2_game_overrides::applyMatching(runtime, "SLUS_203.18", 0x100008u, crc, true);
    }
    R5900Context context(uint32_t blocking, uint32_t command, std::array<uint32_t, 6> args = {})
    {
        R5900Context ctx;
        for (unsigned i = 0; i < 32; ++i) ctx.registers[i] = 0x90000000u + i;
        ctx.registers[4] = blocking;
        ctx.registers[5] = command;
        for (unsigned i = 0; i < 6; ++i) ctx.registers[6 + i] = args[i];
        ctx.registers[29] = kCallerStack;
        ctx.registers[31] = 0x21C568u;
        ctx.pc = command ? kRemote : kInit;
        return ctx;
    }
    int32_t invoke(PS2Runtime &runtime, R5900Context &ctx, uint32_t address = kRemote)
    {
        runtime.lookupFunction(address)(nullptr, &ctx, &runtime);
        return static_cast<int32_t>(ctx.registers[2]);
    }
    void guards()
    {
        for (auto item : kRemoteInstructions)
        {
            PS2Runtime runtime; prepare(runtime);
            runtime.ee.words[item[0]] ^= 1;
            const auto words = runtime.ee.words;
            apply(runtime);
            check(runtime.lookupFunction(kInit) == &original && runtime.lookupFunction(kRemote) == &original,
                  "opcode mismatch installed remote bridge");
            check(runtime.lookupFunction(0x228690u) != &original, "remote guard disabled independent heap fix");
            check(runtime.ee.words == words, "registration changed original guest memory");
        }
        for (uint32_t address : {kInit, kRemote, kSifInit, kBind, kCall})
        {
            PS2Runtime runtime; prepare(runtime); runtime.functions.erase(address); apply(runtime);
            check(!runtime.hasFunction(address), "missing entry created by bridge");
            if (address != kInit) check(runtime.lookupFunction(kInit) == &original, "partial missing-entry install");
            if (address != kRemote) check(runtime.lookupFunction(kRemote) == &original, "partial missing-entry install");
        }
        PS2Runtime runtime; prepare(runtime); runtime.failReplacementAddress = kRemote; apply(runtime);
        check(runtime.lookupFunction(kInit) == &original, "failed second replacement did not roll back init");
        check(runtime.lookupFunction(kRemote) == &original, "failed second replacement changed remote");
        check(runtime.lookupFunction(0x228690u) != &original, "rollback disabled independent heap fix");
        PS2Runtime metadata; prepare(metadata); apply(metadata, 1);
        check(metadata.replacementAttempts == 0u, "wrong CRC installed SDK hooks");
    }
    void packetAndContext()
    {
        PS2Runtime runtime; prepare(runtime); apply(runtime);
        auto init = context(0, 0); const auto initBefore = init.registers;
        check(invoke(runtime, init, kInit) == 1, "retail init success must return one");
        check(sif.inits == 1 && sif.binds == 1 && sif.calls == 0, "wrong init/bind lifecycle");
        check(init.pc == initBefore[31], "init failed to return to RA");
        auto ctx = context(1, 0x8010u, {0x183u, 0u, 0u, 0u, 0u, 0u});
        const auto before = ctx.registers;
        check(invoke(runtime, ctx) == sif.response, "remote response not propagated");
        check(sif.binds == 1 && sif.calls == 1, "each remote call rebound client");
        check(sif.registers[4] == kClient && sif.registers[5] == 0x8010u, "wrong SIF client/function");
        check(sif.registers[6] == 0u, "blocking remote call uses NOWAIT mode");
        check(sif.registers[7] == kPacket && sif.registers[8] == 64u, "request is not retail 64-byte packet");
        check(sif.registers[9] == kPacket && sif.registers[10] == 16u, "response is not retail 16-byte packet");
        check(sif.registers[11] == 0u && sif.endParameter == kPacket, "callback/end parameter differs from retail");
        check(sif.packet[0] == kPacket && sif.packet[1] == 0x183u, "packet pointer/first argument malformed");
        for (unsigned i = 2; i < 7; ++i) check(sif.packet[i] == 0u, "zero remote argument replaced by stack garbage");
        for (unsigned i = 0; i < 32; ++i) if (i != 2) check(ctx.registers[i] == before[i], "remote mutated preserved register");
        for (unsigned offset = 0; offset < 64; offset += 4)
            check(runtime.ee.read32(kCallerStack + offset) == 0x500000u + offset, "caller live stack mutated");
        check(ctx.pc == before[31], "remote failed to return to RA");
        check(runtime.guestAllocations.empty() && runtime.guestAllocationCount == runtime.guestFreeCount,
              "SDK call leaked guest scratch allocation");
        sif.response = -7;
        ctx = context(1, 0x80E0u, {0, 0x10, 0x80000, 0x6000, 0, 0});
        check(invoke(runtime, ctx) == -7, "negative SDRDRV operation response lost");
        check(sif.packet[1] == 0 && sif.packet[2] == 0x10 && sif.packet[3] == 0x80000 && sif.packet[4] == 0x6000,
              "movie AutoDMA loop packet differs from retail");
        ctx = context(1, 0x80E0u, {0, 2, 0, 0, 0, 0}); invoke(runtime, ctx);
        check(sif.packet[2] == 2 && sif.packet[3] == 0 && sif.packet[4] == 0, "movie stop zeros corrupted");
        ctx = context(0, 0x8020u, {0x183, 0, 0, 0, 0, 0}); invoke(runtime, ctx);
        check(sif.registers[6] == 1u, "nonblocking callback-free call lost NOWAIT mode");
        ctx = context(1, 0x8130u, {1, 0x510000, 0, 0, 0, 0});
        check(invoke(runtime, ctx) == static_cast<int32_t>(kPacket), "retail SetEffectAttr return changed");
        check(sif.registers[5] == 0x8131 && sif.registers[7] == 0x510000 &&
              sif.registers[8] == 64 && sif.registers[9] == 0 && sif.registers[10] == 0 &&
              sif.endParameter == kPacket, "SetEffectAttr SIF pack differs from retail");
        ctx = context(1, 0x8140u, {1, 0x520000, 0, 0, 0, 0});
        const uint32_t preservedS1 = ctx.registers[17];
        check(invoke(runtime, ctx) == static_cast<int32_t>(preservedS1), "retail GetEffectAttr preserved-S1 return changed");
        check(sif.registers[5] == 0x8141 && sif.registers[7] == kPacket && sif.registers[8] == 64 &&
              sif.registers[9] == 0x520000 && sif.registers[10] == 64 && sif.endParameter == 0x520000,
              "GetEffectAttr destination/end parameter differs from retail");
    }
    void errors()
    {
        for (unsigned failure = 0; failure < 6; ++failure)
        {
            PS2Runtime runtime; prepare(runtime); apply(runtime);
            if (failure == 0) runtime.physicalRemoteServer = false;
            if (failure == 1) sif.initFailure = true;
            if (failure == 2) sif.bindFailure = true;
            if (failure == 3) sif.emptyServer = true;
            if (failure == 4) runtime.failGuestAllocation = true;
            if (failure == 5) runtime.ee.failWrites = true;
            auto ctx = context(0, 0);
            check(invoke(runtime, ctx, kInit) == -1, "failed init/bind reported success");
            check(sif.calls == 0, "failed bind called sound driver");
            check(runtime.guestAllocations.empty(), "failed init leaked scratch stack");
            if (failure == 0) check(sif.binds == 0 && runtime.scheduler.accountedCycles == 10240000u,
                                    "missing physical IRX used generic bind or failed to advance startup");
        }
        PS2Runtime runtime; prepare(runtime); apply(runtime);
        runtime.physicalRemoteServer = false;
        auto ctx = context(1, 0x8010, {0x183, 0, 0, 0, 0, 0});
        check(invoke(runtime, ctx) == -1 && sif.binds == 0 && sif.calls == 0,
              "missing physical SDRDRV routed to guessed native sound backend");
        runtime.physicalRemoteServer = true; sif.callFailure = true;
        check(invoke(runtime, ctx) == -1, "SIF error ignored in favor of packet self pointer");
        check(runtime.guestAllocations.empty(), "failed command leaked scratch stack");
        sif.callFailure = false;
        for (uint32_t command : {0x8110u, 0x8120u, 0x8160u, 0x8170u})
        {
            const unsigned count = sif.calls;
            ctx = context(1, command);
            check(invoke(runtime, ctx) == -1 && sif.calls == count, "unsupported callback registration silently succeeded");
        }
        runtime.ee.words[0x43AA20u] = 0x123400u;
        const unsigned count = sif.calls;
        ctx = context(0, 0x8010u);
        check(invoke(runtime, ctx) == -1 && sif.calls == count, "unsupported async EE callback silently succeeded");
    }
}
namespace ps2_kfiv_input { void registerOverrides() {} }
namespace ps2_stubs
{
    void sceSifFreeIopHeap(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    { setReturnS32(ctx, runtime->freeIopMemory(getRegU32(ctx, 4)) ? 0 : -1); }
}
int main()
{
    guards(); packetAndContext(); errors();
    std::cout << (failures ? "FAIL: " : "PASS: ") << checks << " assertions, " << failures << " failures\n";
    return failures ? 1 : 0;
}
