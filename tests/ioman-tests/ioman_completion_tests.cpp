#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_ioman.h"
#include "emulator/services/iop_rpc.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace ps2x::iop::detail;

namespace
{
    int checks = 0;
    void require(bool condition, const char *message)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(message);
    }
    class Executor final : public IopGuestExecutor
    {
        uint32_t executeGuestFunction(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override
        {
            throw std::runtime_error("Host file operations unexpectedly invoked guest code");
        }
    };
    int32_t invoke(IopIoman &ioman, Executor &executor, uint16_t ordinal,
                   uint32_t a0, uint32_t a1 = 0u, uint32_t a2 = 0u)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = a0;
        cpu.gpr[5] = a1;
        cpu.gpr[6] = a2;
        require(ioman.dispatchImport(ordinal, cpu, executor), "IOMAN import was not handled");
        return static_cast<int32_t>(cpu.gpr[2]);
    }
}

int main()
{
    const std::filesystem::path filePath = "ioman-completion-test.dat";
    IopMemory memory;
    IopIoman ioman(memory);
    Executor executor;
    try
    {
        const std::string payload = "KFIV asynchronous disc read";
        {
            std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(file), "Cannot create test file");
            file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            require(static_cast<bool>(file), "Cannot write test file");
        }
        constexpr uint32_t pathAddress = 0x1000u;
        const std::string guestPath = "host:ioman-completion-test.dat";
        require(memory.writeRam(pathAddress, guestPath.c_str(), guestPath.size() + 1u), "Cannot write guest path");
        const int32_t fd = invoke(ioman, executor, 4, pathAddress, 0x8001u);
        require(fd > 0, "O_NOWAIT file open failed");
        const uint32_t status = memory.allocate(16u);
        const uint32_t buffer = memory.allocate(64u);
        require(status != 0u && buffer != 0u, "Cannot allocate test outputs");

        for (const uint32_t segment : {0u, 0x80000000u, 0xA0000000u})
        {
            memory.write32(status, 0xA5A5A5A5u);
            memory.write32(status + 4u, 0xDEADBEEFu);
            require(invoke(ioman, executor, 9, fd, 1, status | segment) == 0,
                    "Completion query failed for valid RAM segment");
            require(memory.read32(status) == 0u, "Completion query did not clear initial nonzero state");
            require(memory.read32(status + 4u) == 0xDEADBEEFu, "Completion query changed adjacent data");
        }
        require(invoke(ioman, executor, 6, fd, buffer, 7u) == 7, "Actual host read returned wrong count");
        char bytes[64]{};
        require(memory.readRam(buffer, bytes, 7u), "Cannot inspect guest read data");
        require(std::memcmp(bytes, payload.data(), 7u) == 0, "Actual host read changed file contents");
        memory.write32(status, 1u);
        require(invoke(ioman, executor, 9, fd, 1, status) == 0 && memory.read32(status) == 0u,
                "Read completion remained pending");
        require(invoke(ioman, executor, 8, fd, 0, 0) == 0, "Actual seek failed");
        memory.write32(status, 1u);
        require(invoke(ioman, executor, 9, fd, 1, status) == 0 && memory.read32(status) == 0u,
                "Seek completion remained pending");
        require(invoke(ioman, executor, 6, fd, buffer, 64u) == static_cast<int32_t>(payload.size()),
                "Short read returned wrong byte count");
        require(memory.readRam(buffer, bytes, payload.size()), "Cannot inspect short-read data");
        require(std::memcmp(bytes, payload.data(), payload.size()) == 0, "Short read changed file contents");

        memory.write32(status, 1u);
        require(invoke(ioman, executor, 9, 0xFFFFFFFFu, 1, status) == -9,
                "Invalid descriptor falsely succeeded");
        require(memory.read32(status) == 1u, "Invalid descriptor changed output");
        require(invoke(ioman, executor, 9, fd, 77u, status) == 0,
                "Unrelated command changed established scoped behavior");
        require(memory.read32(status) == 1u, "Unrelated command changed completion output");

        memory.write32(0u, 0xC0FFEEu);
        memory.write32(0x1FFFFCu, 0xFEEDFACEu);
        for (const uint32_t address : {0u, 0x20001000u, 0x1FFFFEu, 0x200000u,
                                       0x10003000u, 0xFFFFFFFFu, 0x4000u})
        {
            require(invoke(ioman, executor, 9, fd, 1, address) == -14,
                    "Invalid completion output falsely succeeded");
            require(memory.read32(0u) == 0xC0FFEEu, "Invalid output modified null RAM");
            require(memory.read32(0x1FFFFCu) == 0xFEEDFACEu, "Invalid output modified end of RAM");
            require(memory.read32(pathAddress) == 0x74736F68u, "Invalid segment aliased guest path");
        }
        require(invoke(ioman, executor, 5, fd) == 0, "Actual close failed");
        memory.write32(status, 1u);
        require(invoke(ioman, executor, 9, fd, 1, status) == -9,
                "Closed descriptor falsely succeeded");
        require(memory.read32(status) == 1u, "Closed descriptor changed output");
        ioman.reset();
        require(std::filesystem::remove(filePath), "Cannot remove closed test file");
        std::cout << "PASS " << checks << " IOMAN completion checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        ioman.reset();
        std::error_code ignored;
        std::filesystem::remove(filePath, ignored);
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
