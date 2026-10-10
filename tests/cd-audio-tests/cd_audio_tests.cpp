#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "ps2x/iop/iop_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace ps2x::iop;
using namespace ps2x::iop::detail;

namespace
{
    unsigned checks = 0;
    void require(bool condition, const char *message)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(message);
    }
    class Host final : public IopHost
    {
    public:
        static constexpr uint32_t mappedStart = 0x00100000u;
        unsigned physicalReads = 0;
        unsigned mappedReads = 0;
        bool mappingFailure = false;
        bool exposeImage = true;
        std::array<uint8_t, 4096> mappedData{};

        Host()
        {
            // Distinctive adjacent sectors prove the read uses the requested
            // file offset, rather than a recent sample or a physical ISO offset.
            std::fill_n(mappedData.begin(), 2048, uint8_t{0x31});
            std::fill_n(mappedData.begin() + 2048, 2048, uint8_t{0xA7});
        }
        std::optional<bool> readMappedCdSectors(uint32_t lsn, uint32_t sectors,
                                               void *destination, size_t size) override
        {
            ++mappedReads;
            if (lsn < mappedStart || lsn >= mappedStart + 2u)
                return std::nullopt;
            const uint64_t offset = static_cast<uint64_t>(lsn - mappedStart) * 2048u;
            if (mappingFailure || sectors * 2048ull != size || offset + size > mappedData.size())
                return false;
            std::memcpy(destination, mappedData.data() + offset, size);
            return true;
        }
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 0u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind kind) const override
        {
            return kind == HostPathKind::CdImage && exposeImage ? "physical.iso" : "";
        }
        std::string translateGuestPath(std::string_view) const override { return {}; }
        uint64_t openHostFile(std::string_view path) override { return path == "physical.iso" ? 1u : 0u; }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t handle, uint64_t, void *destination, size_t size, size_t &bytesRead) override
        {
            require(handle == 1u, "Unexpected physical handle");
            ++physicalReads;
            // Model a valid disc sector with different bytes. The pre-fix
            // implementation accepted these unrelated bytes for synthetic LSNs.
            std::memset(destination, 0xE2, size);
            bytesRead = size;
            return true;
        }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return -1; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}
    };

    uint32_t read(IopCdvd &cdvd, uint32_t lsn, uint32_t sectors, uint32_t destination)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = lsn;
        cpu.gpr[5] = sectors;
        cpu.gpr[6] = destination;
        require(cdvd.dispatchImport(6u, cpu), "sceCdRead import not handled");
        return cpu.gpr[2];
    }
}

int main()
{
    try
    {
        Host host;
        IopMemory memory;
        IopKernel kernel(memory);
        IopCdvd cdvd(host, memory, kernel);
        const uint32_t destination = memory.allocate(8192u);
        require(destination != 0u, "Cannot allocate guest output");

        require(read(cdvd, Host::mappedStart, 2u, destination) == 1u, "Mapped stream read failed");
        std::array<uint8_t, 4096> output{};
        require(memory.readRam(destination, output.data(), output.size()), "Cannot inspect guest output");
        require(output == host.mappedData, "Mapped stream received physical ISO bytes");
        require(host.physicalReads == 0u, "Mapped stream touched physical ISO");
        require(read(cdvd, Host::mappedStart + 1u, 1u, destination | 0xA0000000u) == 1u,
                "Mapped subrange read failed through RAM alias");
        require(memory.read32(destination) == 0xA7A7A7A7u, "Mapped stream offset was lost");

        host.mappingFailure = true;
        memory.write32(destination, 0x12345678u);
        require(read(cdvd, Host::mappedStart, 1u, destination) == 0u,
                "Failed mapped read falsely succeeded through image fallback");
        require(host.physicalReads == 0u, "Failed mapped read touched physical ISO");
        require(memory.read32(destination) == 0x12345678u, "Failed mapped read changed guest output");
        IopCpuState error{};
        require(cdvd.dispatchImport(8u, error) && error.gpr[2] == 0x30u,
                "Failed mapped read did not report disc error");
        require(read(cdvd, Host::mappedStart + 1u, 2u, destination) == 0u,
                "Mapped read crossed extracted-file extent");
        require(host.physicalReads == 0u, "Invalid mapped extent fell back to image");

        require(read(cdvd, 16u, 1u, destination) == 1u, "Unmapped physical sector failed");
        require(host.physicalReads == 1u && memory.read32(destination) == 0xE2E2E2E2u,
                "Physical ISO path changed for unmapped LSN");
        const unsigned mapCalls = host.mappedReads;
        require(read(cdvd, Host::mappedStart, 2u, IopMemory::RamSize - 2048u) == 0u,
                "Out-of-bounds destination succeeded");
        require(host.mappedReads == mapCalls, "Invalid destination reached host reader");
        host.mappingFailure = false;
        require(read(cdvd, Host::mappedStart, 0u, destination) == 1u,
                "Zero-sector read changed existing success semantics");
        require(host.mappedReads == mapCalls, "Zero-sector read reached host reader");
        require(cdvd.dispatchImport(8u, error) && error.gpr[2] == 0u,
                "Successful zero read did not clear previous error");

        cdvd.reset();
        require(read(cdvd, Host::mappedStart, 1u, destination) == 1u,
                "CDVD reset broke host shared mapping");
        require(host.physicalReads == 1u, "CDVD reset bypassed mapped sectors");
        std::cout << "Passed " << checks << " CD audio mapping checks\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
