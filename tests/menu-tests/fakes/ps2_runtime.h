#pragma once

#include "emulator/core/iop_memory.h"
#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

struct R5900Context
{
    std::array<uint32_t, 32> registers{};
    uint32_t pc = 0;
};

inline uint32_t getRegU32(const R5900Context *ctx, int reg)
{
    return reg > 0 && reg < 32 ? ctx->registers[reg] : 0u;
}

inline void setReturnS32(R5900Context *ctx, int32_t result)
{
    ctx->registers[2] = static_cast<uint32_t>(result);
}

class TestEeMemory
{
public:
    uint32_t read32(uint32_t address) const
    {
        if (failReads)
            throw std::runtime_error("test: EE memory unavailable");
        const auto item = words.find(address);
        return item == words.end() ? 0u : item->second;
    }

    void write32(uint32_t address, uint32_t value)
    {
        if (failWrites)
            throw std::runtime_error("test: EE memory write failed");
        words[address] = value;
    }

    std::unordered_map<uint32_t, uint32_t> words;
    bool failReads = false;
    bool failWrites = false;
};

class EeScheduler
{
public:
    void accountCycles(uint64_t cycles) { accountedCycles += cycles; }
    uint64_t accountedCycles = 0;
};

class PS2Runtime
{
public:
    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    TestEeMemory &memory() { return ee; }
    bool hasFunction(uint32_t address) const { return lookupFunction(address) != nullptr; }
    RecompiledFunction lookupFunction(uint32_t address) const
    {
        const auto item = functions.find(address);
        return item == functions.end() ? nullptr : item->second;
    }
    bool replaceFunction(uint32_t address, RecompiledFunction function)
    {
        ++replacementAttempts;
        if (failReplacement || address == failReplacementAddress)
            return false;
        functions[address] = function;
        return true;
    }
    bool freeIopMemory(uint32_t address) { return iop.freeAllocation(address); }
    uint32_t allocateIopMemory(uint32_t size) { return iop.allocate(size, 64u); }
    uint32_t guestMalloc(uint32_t size, uint32_t alignment)
    {
        if (failGuestAllocation) return 0;
        const uint32_t address = (nextGuestAllocation + alignment - 1) & ~(alignment - 1);
        nextGuestAllocation = address + size;
        guestAllocations.insert(address);
        ++guestAllocationCount;
        return address;
    }
    void guestFree(uint32_t address)
    {
        guestAllocations.erase(address);
        ++guestFreeCount;
    }
    EeScheduler &eeScheduler() { return scheduler; }
    ps2x::iop::RpcAbi selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &) const
    { return ps2x::iop::RpcAbi::RuntimeDefault; }
    bool canBindIopRpc(uint32_t) const { return true; }
    bool hasEmulatedIopRpcServer(uint32_t sid) const noexcept
    { return sid == 0x80000701u && physicalRemoteServer; }
    ps2x::iop::RpcResult handleIopRpc(uint8_t *, R5900Context *, ps2x::iop::RpcRequest)
    { return {}; }
    void notifyIopSifTransfer(uint8_t *, const ps2x::iop::SifTransfer &) {}
    void resetIop() {}

    TestEeMemory ee;
    ps2x::iop::detail::IopMemory iop;
    std::unordered_map<uint32_t, RecompiledFunction> functions;
    uint32_t replacementAttempts = 0;
    bool failReplacement = false;
    uint32_t failReplacementAddress = 0;
    bool physicalRemoteServer = false;
    bool failGuestAllocation = false;
    uint32_t nextGuestAllocation = 0x01F00000u;
    uint32_t guestAllocationCount = 0;
    uint32_t guestFreeCount = 0;
    std::unordered_set<uint32_t> guestAllocations;
    EeScheduler scheduler;
};
