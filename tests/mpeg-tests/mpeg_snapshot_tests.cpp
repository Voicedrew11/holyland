#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
unsigned checks = 0u;
void check(bool value, const char *message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
uint32_t word(uint8_t *ram, uint32_t addr)
{
    uint32_t value = 0u; std::memcpy(&value, ram + addr, sizeof(value)); return value;
}
constexpr uint32_t mpeg = 0x120000u, work = 0x130000u;
constexpr uint32_t firstPc = 0x110000u, afterFirstPc = firstPc + 4u;
constexpr uint32_t retryPc = firstPc + 8u, callbackPc = firstPc + 12u;
uint32_t source = 0u;
std::vector<uint8_t> packets;
std::array<unsigned, 3u> calls{}, accepted{};
unsigned completed = 0u;
void *protectedPage = nullptr;
DWORD oldProtection = 0u;

void unprotect()
{
    if (protectedPage)
    {
        DWORD ignored = 0u;
        check(VirtualProtect(protectedPage, 4096u, oldProtection, &ignored) != 0,
              "source tail protection restores before its accepted snapshot");
        protectedPage = nullptr;
    }
}
void demux(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime, uint32_t bytes)
{
    reg(*ctx, 4, mpeg); reg(*ctx, 5, source); reg(*ctx, 6, bytes);
    ps2_stubs::sceMpegDemuxPss(ram, ctx, runtime);
}
void first(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    ctx->pc = afterFirstPc;
    demux(ram, ctx, runtime, 18u);
}
void afterFirst(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(getRegU32(ctx, 2) == 0u && calls[1] == 1u,
          "first genuine callback rejection establishes the pending packet");
    const uint32_t offset = (((source + 4096u) & PS2_RAM_MASK) + 4095u) & ~4095u;
    protectedPage = ram + offset;
    check(VirtualProtect(protectedPage, 4096u, PAGE_NOACCESS, &oldProtection) != 0,
          "later offered bytes become inaccessible during rejected retries");
    ctx->pc = retryPc;
    demux(ram, ctx, runtime, static_cast<uint32_t>(packets.size()));
}
void retry(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (calls[1] < 9u)
    {
        check(getRegU32(ctx, 2) == 0u && protectedPage != nullptr,
              "repeated rejection reads only the known pending packet, preserving the inaccessible tail");
        check(calls[2] == 0u && accepted[1] == 0u, "later packets cannot pass a rejected packet");
        ctx->pc = retryPc;
        demux(ram, ctx, runtime, static_cast<uint32_t>(packets.size()));
        return;
    }
    check(protectedPage == nullptr, "acceptance restores tail access before lazy snapshot extension");
    check(getRegU32(ctx, 2) == packets.size(), "accepted retry processes the entire appended offered tail");
    check(calls[1] == 9u && accepted[1] == 1u && calls[2] == 1u && accepted[2] == 1u,
          "accepted packet and appended audio are each delivered once without loss or replay");
    check(word(ram, work) == 1u, "accepted appended program-end completes the raw SDK lifecycle");
    ++completed; ctx->pc = 0u; runtime->requestStop();
}
void callback(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t tuple = getRegU32(ctx, 5);
    const uint32_t payload = word(ram, tuple + 8u);
    check(word(ram, tuple) == 1u && word(ram, tuple + 12u) == 9u,
          "bounded snapshot keeps actual private-audio callback metadata");
    std::array<uint8_t, 9u> data{};
    for (size_t i = 0u; i < data.size(); ++i) data[i] = ram[(payload + i) & PS2_RAM_MASK];
    const unsigned id = data[4];
    check(id == 1u || id == 2u, "original payload identity survives linear and physical-RAM wrapping");
    check(data[0] == 0xFFu && data[1] == 0xA0u && data[5] == 0x10u && data[8] == 0x40u,
          "original guest payload remains intact across every retry");
    ++calls[id];
    const bool accept = id == 2u || calls[id] == 9u;
    if (accept)
    {
        if (id == 1u) unprotect();
        ++accepted[id];
    }
    reg(*ctx, 2, accept ? 1u : 0u); ctx->pc = 0u;
}
void runCase(uint32_t address)
{
    source = address; calls = {}; accepted = {}; completed = 0u;
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "real Windows guest memory initializes");
    auto *ram = runtime->memory().getRDRAM();
    ps2_stubs::resetMpegStubState(); ps2_stubs::notifyMpegCdStreamStart(runtime.get());
    R5900Context create{}; reg(create, 4, mpeg); reg(create, 5, work); reg(create, 6, 0x2000u);
    ps2_stubs::sceMpegCreate(ram, &create, runtime.get());
    for (size_t i = 0u; i < packets.size(); ++i) ram[(source + i) & PS2_RAM_MASK] = packets[i];
    runtime->registerFunction(firstPc, first); runtime->registerFunction(afterFirstPc, afterFirst);
    runtime->registerFunction(retryPc, retry); runtime->registerFunction(callbackPc, callback);
    R5900Context add{}; reg(add, 4, mpeg); reg(add, 5, 1u); reg(add, 7, callbackPc);
    ps2_stubs::sceMpegAddStrCallback(ram, &add, runtime.get());
    ps2_stubs::notifyMpegCdStreamDataProduced(static_cast<uint32_t>(packets.size()), true);
    R5900Context main{}; main.pc = firstPc;
    runtime->eeScheduler().reset(ram, main); runtime->eeScheduler().run();
    check(completed == 1u, "bounded retries and appended-tail producer resume normally");
}
}

int main()
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try
    {
        const auto packet = [](uint8_t id)
        {
            return std::array<uint8_t, 18u>{0u, 0u, 1u, 0xBDu, 0u, 12u, 0x80u, 0u, 0u,
                0xFFu, 0xA0u, 0u, 0u, id, 0x10u, 0x20u, 0x30u, 0x40u};
        };
        auto firstPacket = packet(1u); packets.insert(packets.end(), firstPacket.begin(), firstPacket.end());
        packets.insert(packets.end(), {0u, 0u, 1u, 0xBEu, 0x20u, 0u});
        packets.insert(packets.end(), 8192u, 0xEEu);
        auto lastPacket = packet(2u); packets.insert(packets.end(), lastPacket.begin(), lastPacket.end());
        packets.insert(packets.end(), {0u, 0u, 1u, 0xB9u});
        runCase(0x140000u);
        runCase(PS2_RAM_SIZE - 13u);
        std::cout << "PASS: " << checks << " actual MPEG bounded retry and bulk snapshot checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        unprotect();
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
