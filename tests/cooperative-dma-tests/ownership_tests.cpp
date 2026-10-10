// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/ps2_memory.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

static unsigned checks = 0;
static void require(bool condition, const char *message)
{ ++checks; if (!condition) throw std::runtime_error(message); }
static void word(std::vector<uint8_t> &bytes, uint32_t value)
{ const auto p = reinterpret_cast<const uint8_t *>(&value); bytes.insert(bytes.end(), p, p + 4); }
static uint32_t cmd(unsigned op, unsigned num = 0, unsigned imm = 0)
{ return op << 24 | num << 16 | imm; }
static std::vector<uint8_t> launch(unsigned pc)
{ std::vector<uint8_t> bytes; word(bytes, cmd(0x14, 0, pc)); while (bytes.size() < 16) word(bytes, 0); return bytes; }

int main()
{
    try
    {
        PS2Memory memory;
        require(memory.initialize(), "actual memory allocation");
        bool active = false;
        unsigned launches = 0;
        const uint8_t *expectedAllocation = nullptr;
        memory.setVu1CooperativeCallbacks([&] { return active; }, [&] { active = false; });
        memory.setVu1MscalCallback([&](uint32_t pc, uint32_t, uint32_t)
        {
            require(!active, "launch remains serialized");
            if (expectedAllocation)
                require(memory.m_vif1Input.data() == expectedAllocation, "empty active parser adopts incoming allocation");
            require(pc == (launches + 1u) * 8u, "ordered program-counter launch");
            ++launches;
            active = true;
        });

        // Exercise the exact already-owned PendingTransfer path independently
        // of the chain-flattening algorithm. No payload may be borrowed after
        // the pending vector and its original owner are destroyed.
        PS2Memory::PendingTransfer first;
        first.chainData = launch(1);
        const uint8_t *firstAllocation = first.chainData.data();
        memory.m_pendingVif1Transfers.push_back(std::move(first));
        memory.processPendingTransfers();
        require(memory.m_pendingVif1Transfers.empty(), "accepted transfer removed");
        require(memory.m_vif1Incoming.size() == 1, "one separately owned incoming chunk");
        require(memory.m_vif1Incoming.front().data() == firstAllocation, "owned chain allocation moved to incoming");
        require(memory.m_vif1AcceptedBytes == 16 && memory.m_vif1DmaEnds.front() == 16, "first accepted marker exact");
        expectedAllocation = firstAllocation;
        memory.serviceVif1Stream();
        require(active && launches == 1, "first payload launches");

        // Callback-time raw/FIFO input still snapshots its producer. It must
        // never append into an active parser allocation or overwrite bytes.
        std::vector<uint8_t> second = launch(2);
        const uint8_t *activeAllocation = memory.m_vif1Input.data();
        memory.processVIF1Data(second.data(), static_cast<uint32_t>(second.size()));
        const uint8_t *secondAllocation = memory.m_vif1Incoming.front().data();
        require(secondAllocation != second.data(), "borrowed raw bytes remain a snapshot");
        std::memset(second.data(), 0, second.size());
        memory.serviceVif1Stream();
        require(memory.m_vif1Input.data() == activeAllocation && launches == 1, "active job preserves parser lifetime");
        active = false;
        // The first launch still has padding after its cursor, so this chunk
        // must append across that existing tail rather than adopt storage.
        expectedAllocation = nullptr;
        memory.serviceVif1Stream();
        require(active && launches == 2, "raw snapshot survives producer mutation");
        active = false;
        expectedAllocation = nullptr;
        memory.serviceVif1Stream();
        require(!memory.vif1WorkPending(), "moved stream fully retired");
        require(memory.consumeCompletedDmacCauses() == std::vector<uint32_t>{1}, "DMA marker retires only once");

        PS2Memory::PendingTransfer third;
        third.chainData = launch(3);
        const uint8_t *thirdAllocation = third.chainData.data();
        memory.m_pendingVif1Transfers.push_back(std::move(third));
        memory.processPendingTransfers();
        expectedAllocation = thirdAllocation;
        memory.serviceVif1Stream();
        require(active && launches == 3, "consumed active allocation replaced by later owned chunk");
        active = false;
        expectedAllocation = nullptr;
        memory.serviceVif1Stream();
        require(!memory.vif1WorkPending(), "later moved stream fully retired");
        memory.consumeCompletedDmacCauses();

        // A command split across two owned DMAs must retain both retirement
        // boundaries while appending to the nonempty partial active buffer.
        memory.writeIORegister(0x1000E010u, 2u);
        memory.cancelVif1Work();
        PS2Memory::PendingTransfer header, payload;
        word(header.chainData, cmd(0x6C, 1, 9));
        const uint32_t literal[] = {0x12345678, 0x89ABCDEF, 0x10203040, 0x50607080};
        for (auto value : literal) word(payload.chainData, value);
        memory.m_pendingVif1Transfers.push_back(std::move(header));
        memory.processPendingTransfers();
        memory.serviceVif1Stream();
        require(memory.vif1WorkPending() && !memory.vif1Runnable(), "partial owned command blocks");
        require(memory.consumeCompletedDmacCauses() == std::vector<uint32_t>{1}, "accepted first boundary retires");
        memory.writeIORegister(0x1000E010u, 2u);
        memory.m_pendingVif1Transfers.push_back(std::move(payload));
        memory.processPendingTransfers();
        require(memory.m_vif1AcceptedBytes == 20 && memory.m_vif1DmaEnds.front() == 20, "second cumulative marker exact");
        memory.serviceVif1Stream();
        require(std::memcmp(memory.getVU1Data() + 9 * 16, literal, sizeof(literal)) == 0, "split owned payload literal values");
        require(memory.consumeCompletedDmacCauses() == std::vector<uint32_t>{1}, "second boundary independently retires");
        require(!memory.vif1WorkPending(), "split moved stream fully retired");
        std::cout << "ownership checks=" << checks << " launches=" << launches << '\n';
        return 0;
    }
    catch (const std::exception &error)
    { std::cerr << error.what() << " checks=" << checks << '\n'; return 1; }
}
