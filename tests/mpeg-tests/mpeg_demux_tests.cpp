#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "Stubs/MPEG.h"

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
uint32_t word(uint8_t *ram, uint32_t addr)
{
    uint32_t value = 0u;
    std::memcpy(&value, ram + addr, sizeof(value));
    return value;
}
void reg(R5900Context &ctx, int index, uint32_t value) { SET_GPR_U32(&ctx, index, value); }
constexpr uint32_t mpeg = 0x00120000u, work = 0x00130000u;
constexpr uint32_t ring = 0x00140000u, ringBytes = 128u, ringOffset = 116u;
constexpr size_t programBytes = 58u;
constexpr uint32_t mainPc = 0x00110000u, retryPc = mainPc + 4u, finishPc = mainPc + 8u;
constexpr uint32_t callbackPc = mainPc + 12u;
std::array<unsigned, 4u> firstCalls{}, secondCalls{}, accepted{};
std::vector<unsigned> order;
std::vector<uint8_t> packets;
unsigned finished = 0u;

uint32_t address(size_t index) { return ring + ((ringOffset + static_cast<uint32_t>(index)) % ringBytes); }
void prepare(R5900Context &ctx, size_t start, size_t bytes)
{
    reg(ctx, 4, mpeg); reg(ctx, 5, address(start)); reg(ctx, 6, static_cast<uint32_t>(bytes));
    reg(ctx, 7, ring); reg(ctx, 8, ringBytes);
}
int query(PS2Runtime *runtime, uint8_t *ram)
{
    R5900Context ctx{}; reg(ctx, 4, mpeg);
    ps2_stubs::sceMpegIsEnd(ram, &ctx, runtime);
    return static_cast<int32_t>(getRegU32(&ctx, 2));
}

void callback(uint8_t *ram, R5900Context *ctx, PS2Runtime *)
{
    check(getRegU32(ctx, 4) == mpeg, "stream callback retains MPEG handle");
    const uint32_t data = getRegU32(ctx, 5);
    const uint32_t type = word(ram, data);
    const uint32_t source = word(ram, data + 8u);
    const uint32_t length = word(ram, data + 12u);
    check(type == 1u || type == 2u, "matching private audio callback type");
    check(getRegU32(ctx, 6) == 0xAA000000u + type, "stream callback retains registered user data");
    check(source >= ring && source < ring + ringBytes, "payload pointer remains in original guest ring");
    check(length == 9u, "private stream prefix retained for retail callback");
    std::array<uint8_t, 9u> payload{};
    for (size_t i = 0u; i < payload.size(); ++i)
        payload[i] = ram[ring + ((source - ring + static_cast<uint32_t>(i)) % ringBytes)];
    check(payload[0] == 0xFFu && payload[1] == 0xA0u && payload[2] == 0u && payload[3] == 0u,
          "wrapped private stream header remains intact until acceptance");
    const unsigned id = payload[4];
    check(id >= 1u && id <= 3u, "payload identity remains stable on retry");
    check(payload[5] == 0x10u && payload[6] == 0x20u && payload[7] == 0x30u && payload[8] == 0x40u,
          "guest producer has not overwritten pending packet bytes");
    order.push_back(type * 10u + id);
    if (type == 1u)
    {
        ++firstCalls[id];
        reg(*ctx, 2, 1u);
    }
    else
    {
        ++secondCalls[id];
        const bool reject = id == 2u && secondCalls[id] == 1u;
        if (!reject) ++accepted[id];
        reg(*ctx, 2, reject ? 0u : 1u);
    }
    ctx->pc = 0u;
}

void mainEntry(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    prepare(*ctx, 0u, 16u);
    ps2_stubs::sceMpegDemuxPssRing(ram, ctx, runtime);
    check(getRegU32(ctx, 2) == 0u, "incomplete PES remains wholly in guest ring");
    check(order.empty(), "incomplete PES does not prematurely invoke callbacks");
    prepare(*ctx, 0u, packets.size());
    ctx->pc = retryPc;
    ps2_stubs::sceMpegDemuxPssRing(ram, ctx, runtime);
}

void retry(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(getRegU32(ctx, 2) == 18u, "rejected second packet commits only first complete packet");
    check(firstCalls[1] == 1u && secondCalls[1] == 1u && accepted[1] == 1u,
          "first accepted packet is delivered exactly once");
    check(firstCalls[2] == 1u && secondCalls[2] == 1u && accepted[2] == 0u,
          "second packet preserves partial callback acceptance on rejection");
    check(firstCalls[3] == 0u && secondCalls[3] == 0u, "later packets wait behind rejected packet");
    check(word(ram, work) == 0u && query(runtime, ram) == 0,
          "producer EOF cannot finish while rejected tail remains");
    ps2_stubs::notifyMpegCdStreamEof(runtime);
    check(word(ram, work) == 0u && query(runtime, ram) == 0,
          "explicit producer EOF also preserves uncommitted tail");
    for (size_t i = 0u; i < 18u; ++i) ram[address(i)] = 0xCCu;
    prepare(*ctx, 18u, packets.size() - 18u);
    ctx->pc = finishPc;
    ps2_stubs::sceMpegDemuxPssRing(ram, ctx, runtime);
}

void finish(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    check(getRegU32(ctx, 2) == programBytes - 18u, "retry commits accepted tail only through program-end marker");
    check(firstCalls[2] == 1u && secondCalls[2] == 2u && accepted[2] == 1u,
          "retry does not duplicate already accepted callback for same packet");
    check(firstCalls[3] == 1u && secondCalls[3] == 1u && accepted[3] == 1u,
          "final audio packet is dispatched before EOF completion");
    check(order == std::vector<unsigned>{11u, 21u, 12u, 22u, 22u, 13u, 23u},
          "callbacks retain sequential order and never parse PES-looking bytes after program end");
    check(word(ram, work) == 1u && query(runtime, ram) == 1,
          "accepted program end completes after all audio callbacks despite unread CD padding");
    R5900Context reset{}; reg(reset, 4, mpeg);
    ps2_stubs::sceMpegReset(ram, &reset, runtime);
    check(word(ram, work) == 0u && query(runtime, ram) == 0,
          "reset clears accepted program end while physical producer EOF is still pending");
    ps2_stubs::notifyMpegCdStreamStart(runtime);
    check(word(ram, work) == 0u && query(runtime, ram) == 0,
          "next CD generation cannot inherit accepted program end");
    ++finished;
    ctx->pc = 0u;
    runtime->requestStop();
}
}

int main()
{
    try
    {
        auto runtime = std::make_unique<PS2Runtime>();
        check(runtime->memory().initialize(), "real guest memory initializes");
        uint8_t *ram = runtime->memory().getRDRAM();
        ps2_stubs::resetMpegStubState();
        ps2_stubs::notifyMpegCdStreamStart(runtime.get());
        R5900Context create{}; reg(create, 4, mpeg); reg(create, 5, work); reg(create, 6, 0x2000u);
        ps2_stubs::sceMpegCreate(ram, &create, runtime.get());
        for (unsigned id = 1u; id <= 3u; ++id)
        {
            const std::array<uint8_t, 18u> packet{0u, 0u, 1u, 0xBDu, 0u, 12u, 0x80u, 0u, 0u,
                0xFFu, 0xA0u, 0u, 0u, static_cast<uint8_t>(id), 0x10u, 0x20u, 0x30u, 0x40u};
            packets.insert(packets.end(), packet.begin(), packet.end());
        }
        packets.insert(packets.end(), {0u, 0u, 1u, 0xB9u});
        // This recognizable private PES follows the authoritative end marker.
        // It must remain outside the ended program and invoke no callbacks.
        const std::vector<uint8_t> trailingPacket(packets.begin() + 36u, packets.begin() + 54u);
        packets.insert(packets.end(), trailingPacket.begin(), trailingPacket.end());
        for (size_t i = 0u; i < packets.size(); ++i) ram[address(i)] = packets[i];
        runtime->registerFunction(mainPc, mainEntry); runtime->registerFunction(retryPc, retry);
        runtime->registerFunction(finishPc, finish); runtime->registerFunction(callbackPc, callback);
        for (uint32_t type : {1u, 2u})
        {
            R5900Context add{}; reg(add, 4, mpeg); reg(add, 5, type); reg(add, 6, 0u);
            reg(add, 7, callbackPc); reg(add, 8, 0xAA000000u + type);
            ps2_stubs::sceMpegAddStrCallback(ram, &add, runtime.get());
            check(getRegU32(&add, 2) == 0u, "stream callback registers");
        }
        // A CD reader reports complete sectors; the MPEG program itself ends
        // before those final sector-padding bytes have been offered to demux.
        ps2_stubs::notifyMpegCdStreamDataProduced(static_cast<uint32_t>(packets.size() + 2044u), true);
        R5900Context ctx{}; ctx.pc = mainPc;
        runtime->eeScheduler().reset(ram, ctx);
        runtime->eeScheduler().run();
        check(finished == 1u, "producer resumes after accepted callback completion");
        std::cout << "PASS: " << checks << " actual MPEG acceptance and EOF checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
