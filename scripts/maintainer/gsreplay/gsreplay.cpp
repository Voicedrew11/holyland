// gsreplay: replay a PS2X_GS_RECORD trace through the current GS CPU backend
// and (optionally) a reference backend, comparing VRAM at every sync point
// and the transfer state after every upload. Build with build.sh.
//   gsreplay <trace> [--no-ref] [--no-new] [--max-ops N]
// Compiled together with PS2Recomp sources (GPL-3.0), so, like patches/,
// it is distributed under the same license.
#include "runtime/gs/gs_cpu_backend.h"
#include "ref/gs_cpu_backend_ref.h"
#include "gs/gs_record.h"
#include "gs/gs_trace.h"

#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The runtime's GS trace is not linked; the backends only query it.
namespace GSTrace
{
    bool enabled() { return false; }
    void imageTransfer(const GSBitBltBuf &, const GSTrxPos &, const GSTrxReg &, uint32_t) {}
}

namespace
{
    struct Reader
    {
        std::FILE *f;
        template <typename T>
        bool get(T &v) { return std::fread(&v, sizeof(T), 1u, f) == 1u; }
        bool bytes(std::vector<uint8_t> &out, uint32_t n)
        {
            out.resize(n);
            return n == 0u || std::fread(out.data(), 1u, n, f) == n;
        }
    };

    using Clock = std::chrono::steady_clock;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <trace> [--no-ref] [--no-new] [--max-ops N]\n", argv[0]);
        return 2;
    }
    bool useRef = true, useNew = true;
    uint64_t maxOps = ~0ull;
    for (int i = 2; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--no-ref") useRef = false;
        else if (a == "--no-new") useNew = false;
        else if (a == "--max-ops" && i + 1 < argc) maxOps = std::strtoull(argv[++i], nullptr, 0);
    }
    Reader in{std::fopen(argv[1], "rb")};
    if (!in.f) { std::perror("open"); return 2; }
    std::setvbuf(in.f, nullptr, _IOFBF, 1u << 22u);

    std::vector<uint8_t> vramNew, vramRef, snapNew, snapRef, payload;
    std::unique_ptr<GSCpuBackend> nb = useNew ? std::make_unique<GSCpuBackend>() : nullptr;
    std::unique_ptr<GSCpuBackendRef> rb = useRef ? std::make_unique<GSCpuBackendRef>() : nullptr;
    double tNew = 0.0, tRef = 0.0;
    uint64_t ops = 0, compares = 0, mismatches = 0, submits = 0;

    auto timed = [](double &acc, auto &&fn) {
        const auto t0 = Clock::now();
        fn();
        acc += std::chrono::duration<double>(Clock::now() - t0).count();
    };
    auto compare = [&](const char *why) {
        if (!nb || !rb) return;
        timed(tNew, [&] { nb->SnapshotVram(snapNew); });
        timed(tRef, [&] { rb->SnapshotVram(snapRef); });
        ++compares;
        if (snapNew.size() != snapRef.size() || std::memcmp(snapNew.data(), snapRef.data(), snapNew.size()) != 0)
        {
            ++mismatches;
            if (mismatches <= 10)
            {
                size_t diffBytes = 0, first = ~size_t(0);
                std::vector<int> pages;
                for (size_t i = 0; i < std::min(snapNew.size(), snapRef.size()); ++i)
                    if (snapNew[i] != snapRef[i])
                    {
                        ++diffBytes;
                        if (first == ~size_t(0)) first = i;
                        if (pages.empty() || pages.back() != int(i / 8192)) pages.push_back(int(i / 8192));
                    }
                std::fprintf(stderr, "MISMATCH at op %llu (%s, after %llu submits): %zu bytes differ, first 0x%zx, pages:",
                             (unsigned long long)ops, why, (unsigned long long)submits, diffBytes, first);
                for (size_t p = 0; p < pages.size() && p < 24; ++p) std::fprintf(stderr, " %d", pages[p]);
                std::fprintf(stderr, "\n");
            }
            // Resynchronise so later mismatches are independent.
            // (in place: re-initializing would reset the reference CLUT).
            std::memcpy(vramRef.data(), snapNew.data(), vramRef.size());
            rb->TextureFlush();
        }
    };

    uint8_t op = 0;
    while (ops < maxOps && in.get(op))
    {
        ++ops;
        switch (static_cast<GSRecord::Op>(op))
        {
        case GSRecord::Op::Initialize:
        {
            uint32_t size = 0;
            if (!in.get(size) || !in.bytes(payload, size)) goto done;
            vramNew = payload; vramRef = payload;
            if (vramNew.size() < (4u << 20)) { vramNew.resize(4u << 20); vramRef.resize(4u << 20); }
            if (nb) nb->Initialize(vramNew.data(), static_cast<uint32_t>(vramNew.size()));
            if (rb) rb->Initialize(vramRef.data(), static_cast<uint32_t>(vramRef.size()));
            break;
        }
        case GSRecord::Op::Reset:
            if (nb) timed(tNew, [&] { nb->Reset(); });
            if (rb) timed(tRef, [&] { rb->Reset(); });
            break;
        case GSRecord::Op::Submit:
        {
            int32_t mode = 0; GSPrimitiveBatch b{};
            if (!in.get(mode) || !in.get(b)) goto done;
            ++submits;
            std::fesetround(mode);
            if (nb) timed(tNew, [&] { nb->Submit(b); });
            if (rb) timed(tRef, [&] { rb->Submit(b); });
            std::fesetround(FE_TONEAREST);
            break;
        }
        case GSRecord::Op::LoadClut:
        {
            GSTex0Reg t{}; GSTexClutReg c{};
            if (!in.get(t) || !in.get(c)) goto done;
            if (nb) timed(tNew, [&] { nb->LoadClut(t, c); });
            if (rb) timed(tRef, [&] { rb->LoadClut(t, c); });
            break;
        }
        case GSRecord::Op::BeginTransfer:
        {
            GSTransferCommand c{};
            if (!in.get(c)) goto done;
            if (nb) timed(tNew, [&] { nb->BeginTransfer(c); });
            if (rb) timed(tRef, [&] { rb->BeginTransfer(c); });
            break;
        }
        case GSRecord::Op::UploadImage:
        {
            uint32_t size = 0;
            if (!in.get(size) || !in.bytes(payload, size)) goto done;
            if (nb) timed(tNew, [&] { nb->UploadImage(payload.data(), size); });
            if (rb) timed(tRef, [&] { rb->UploadImage(payload.data(), size); });
            if (nb && rb)
            {
                const GSTransferSnapshot a = nb->GetTransferSnapshot(), b2 = rb->GetTransferSnapshot();
                if (a.x != b2.x || a.y != b2.y || a.totalPixels != b2.totalPixels || a.copiedPixels != b2.copiedPixels || a.direction != b2.direction)
                {
                    ++mismatches;
                    if (mismatches <= 10)
                        std::fprintf(stderr, "MISMATCH transfer state at op %llu size %u: new x%u y%u tot%u cp%u dir%u | ref x%u y%u tot%u cp%u dir%u\n", (unsigned long long)ops, size, a.x, a.y, a.totalPixels, a.copiedPixels, a.direction, b2.x, b2.y, b2.totalPixels, b2.copiedPixels, b2.direction);
                }
            }
            break;
        }
        case GSRecord::Op::Flush:
            if (nb) timed(tNew, [&] { nb->Flush(); });
            if (rb) timed(tRef, [&] { rb->Flush(); });
            break;
        case GSRecord::Op::TextureFlush:
            if (nb) timed(tNew, [&] { nb->TextureFlush(); });
            if (rb) timed(tRef, [&] { rb->TextureFlush(); });
            break;
        case GSRecord::Op::Sync:
        {
            uint8_t reason = 0;
            if (!in.get(reason)) goto done;
            if (nb) timed(tNew, [&] { nb->Sync(static_cast<GSSyncReason>(reason)); });
            if (rb) timed(tRef, [&] { rb->Sync(static_cast<GSSyncReason>(reason)); });
            break;
        }
        case GSRecord::Op::Present:
        {
            GSPresentationRequest r{};
            if (!in.get(r)) goto done;
            compare("present");
            break;
        }
        case GSRecord::Op::ClearFramebuffer:
        {
            GSContext c{}; uint32_t rgba = 0;
            if (!in.get(c) || !in.get(rgba)) goto done;
            if (nb) timed(tNew, [&] { nb->ClearFramebuffer(c, rgba); });
            if (rb) timed(tRef, [&] { rb->ClearFramebuffer(c, rgba); });
            break;
        }
        case GSRecord::Op::ConsumeLocalToHost:
        {
            uint32_t n = 0;
            if (!in.get(n)) goto done;
            std::vector<uint8_t> a(n), b(n);
            uint32_t na = nb ? nb->ConsumeLocalToHostBytes(a.data(), n) : 0u;
            uint32_t nr = rb ? rb->ConsumeLocalToHostBytes(b.data(), n) : 0u;
            if (nb && rb && (na != nr || std::memcmp(a.data(), b.data(), na) != 0))
                std::fprintf(stderr, "MISMATCH local-to-host bytes at op %llu\n", (unsigned long long)ops);
            break;
        }
        case GSRecord::Op::ReadVram:
        {
            uint32_t v[5];
            for (auto &x : v) if (!in.get(x)) goto done;
            compare("readvram");
            break;
        }
        case GSRecord::Op::WriteVram:
        {
            uint32_t v[6];
            for (auto &x : v) if (!in.get(x)) goto done;
            if (nb) nb->WriteVram(v[0], v[1], v[2], v[3], v[4], v[5]);
            if (rb) rb->WriteVram(v[0], v[1], v[2], v[3], v[4], v[5]);
            break;
        }
        case GSRecord::Op::SnapshotVram:
            compare("snapshot");
            break;
        default:
            std::fprintf(stderr, "bad op %u at %llu\n", op, (unsigned long long)ops);
            goto done;
        }
    }
done:
    if (nb) timed(tNew, [&] { nb->Sync(GSSyncReason::DebugReadback); });
    compare("end");
    std::printf("ops %llu submits %llu compares %llu mismatches %llu | new %.2f s  ref %.2f s\n",
                (unsigned long long)ops, (unsigned long long)submits, (unsigned long long)compares,
                (unsigned long long)mismatches, tNew, tRef);
    return mismatches ? 1 : 0;
}
