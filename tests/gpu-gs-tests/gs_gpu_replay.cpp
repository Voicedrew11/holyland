// SPDX-License-Identifier: GPL-3.0-only
// Source-only replay utility for PS2Recomp's opt-in GSRecord stream.
// A recording is private game data and must never be included in this package.
#include "runtime/gs/gs_cpu_backend.h"
#ifndef PS2X_REPLAY_CPU_ONLY
#include "runtime/gs/gs_vulkan_backend.h"
#endif
#include "gs_record.h"
#include "gs_trace.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace GSTrace {
bool enabled() { return false; }
void imageTransfer(const GSBitBltBuf &, const GSTrxPos &, const GSTrxReg &, uint32_t) {}
}

namespace {
using Clock = std::chrono::steady_clock;
struct Reader {
    std::FILE *file = nullptr;
    std::vector<uint8_t> cache;
    size_t cursor = 0;
    bool cached = false;
    explicit Reader(const char *path) : file(std::fopen(path, "rb")) {
        if (!file) throw std::runtime_error("Cannot open private GS recording");
        std::setvbuf(file, nullptr, _IOFBF, 4u << 20);
    }
    ~Reader() { if (file) std::fclose(file); }
    void readData(void *dst, size_t size) {
        if (cached) {
            if (size > cache.size() - cursor)
                throw std::runtime_error("Truncated cached recording payload");
            if (size) std::memcpy(dst, cache.data() + cursor, size);
            cursor += size;
        } else if (size && std::fread(dst, 1, size, file) != size) {
            throw std::runtime_error("Truncated recording payload / incompatible GSRecord ABI");
        }
    }
    template<class T> T read() {
        T value{};
        readData(&value, sizeof value);
        return value;
    }
    bool next(uint8_t &op) {
        if (cached) {
            if (cursor == cache.size()) return false;
            op = cache[cursor++]; return true;
        }
        if (std::fread(&op, 1, 1, file) == 1) return true;
        if (std::ferror(file)) throw std::runtime_error("Recording read failed");
        return false;
    }
    std::vector<uint8_t> bytes(uint32_t size) {
        if (size > (64u << 20)) throw std::runtime_error("Invalid payload size / incompatible GSRecord ABI");
        std::vector<uint8_t> result(size);
        readData(result.data(), size);
        return result;
    }
    void cacheWindow(uint64_t untilTick, uint64_t remainingOps, size_t limit) {
        // Parse to a presentation boundary before starting the benchmark.
        // Caching the native bytes retains exactly the streaming replay ABI.
        cache.reserve(limit);
        auto append = [&](const void *data, size_t size) {
            if (!size) return;
            if (size > limit - cache.size()) throw std::runtime_error("Cached window exceeds --cache-window-mib limit");
            const auto *p = static_cast<const uint8_t *>(data);
            cache.insert(cache.end(), p, p + size);
        };
        auto payload = [&]<class T>() {
            const auto value = read<T>(); append(&value, sizeof value); return value;
        };
        for (uint64_t n = 0; n < remainingOps; ++n) {
            uint8_t opcode;
            if (!next(opcode)) break;
            append(&opcode, 1);
            switch (static_cast<GSRecord::Op>(opcode)) {
            case GSRecord::Op::Initialize:
            case GSRecord::Op::UploadImage: {
                const auto size = payload.operator()<uint32_t>();
                const auto data = bytes(size); append(data.data(), data.size()); break;
            }
            case GSRecord::Op::Submit:
                payload.operator()<int32_t>(); payload.operator()<GSPrimitiveBatch>(); break;
            case GSRecord::Op::LoadClut:
                payload.operator()<GSTex0Reg>(); payload.operator()<GSTexClutReg>(); break;
            case GSRecord::Op::BeginTransfer: payload.operator()<GSTransferCommand>(); break;
            case GSRecord::Op::Sync: payload.operator()<uint8_t>(); break;
            case GSRecord::Op::Present:
                if (payload.operator()<GSPresentationRequest>().vsyncTick >= untilTick) {
                    cached = true; return;
                }
                break;
            case GSRecord::Op::ClearFramebuffer:
                payload.operator()<GSContext>(); payload.operator()<uint32_t>(); break;
            case GSRecord::Op::ConsumeLocalToHost: payload.operator()<uint32_t>(); break;
            case GSRecord::Op::ReadVram: payload.operator()<std::array<uint32_t,5>>(); break;
            case GSRecord::Op::WriteVram: payload.operator()<std::array<uint32_t,6>>(); break;
            case GSRecord::Op::Reset:
            case GSRecord::Op::Flush:
            case GSRecord::Op::TextureFlush:
            case GSRecord::Op::SnapshotVram: break;
            default: throw std::runtime_error("Unknown opcode while caching window");
            }
        }
        throw std::runtime_error("Cached recording ends before the requested presentation boundary");
    }
};
struct Run {
    const char *name;
    std::unique_ptr<GSRasterBackend> backend;
    std::vector<uint8_t> vram;
    double allSeconds = 0, measuredSeconds = 0, initializeSeconds = 0;
    PresentationFrame frame;
    template<class F> void call(bool measured, F &&fn) {
        const auto start = Clock::now();
        fn(*backend);
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        allSeconds += elapsed;
        if (measured) measuredSeconds += elapsed;
    }
};
size_t frameStride(const PresentationFrame &frame) {
    // Runtime presentation uses a fixed 640-pixel host row even when its
    // logical viewport is narrower. Also accept compact imported frames.
    return frame.pixels.size() >= size_t(640) * frame.height * 4 ? 640 : frame.width;
}
void writePpm(const std::filesystem::path &path, const PresentationFrame &frame) {
    if (!frame) return;
    std::FILE *file = nullptr;
#ifdef _WIN32
    file = _wfopen(path.c_str(), L"wb");
#else
    file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) throw std::runtime_error("Cannot create private frame output");
    std::fprintf(file, "P6\n%u %u\n255\n", frame.width, frame.height);
    const auto stride = frameStride(frame);
    if (frame.pixels.size() < stride * frame.height * 4)
        throw std::runtime_error("Presentation has an invalid pixel buffer");
    for (size_t y = 0; y < frame.height; ++y)
        for (size_t x = 0; x < frame.width; ++x)
            std::fwrite(frame.pixels.data() + 4 * (y * stride + x), 1, 3, file);
    if (std::fclose(file) != 0) throw std::runtime_error("Frame output write failed");
}
struct Difference {
    uint64_t frames = 0, pixels = 0, exact = 0, aboveEight = 0;
    uint64_t absolute = 0, squared = 0;
    uint8_t maximum = 0;
    bool dimensionsDiffer = false;
    void add(const PresentationFrame &a, const PresentationFrame &b) {
        ++frames;
        if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size()) {
            dimensionsDiffer = true;
            return;
        }
        const auto strideA = frameStride(a), strideB = frameStride(b);
        const size_t count = size_t(a.width) * a.height;
        if (a.pixels.size() < strideA * a.height * 4 || b.pixels.size() < strideB * b.height * 4)
            throw std::runtime_error("Invalid presentation buffer in image comparison");
        pixels += count;
        for (size_t i = 0; i < count; ++i) {
            unsigned localMax = 0;
            for (size_t c = 0; c < 3; ++c) {
                const size_t x = i % a.width, y = i / a.width;
                const auto delta = unsigned(std::abs(int(a.pixels[4*(y*strideA+x)+c]) - int(b.pixels[4*(y*strideB+x)+c])));
                absolute += delta; squared += delta * delta;
                localMax = std::max(localMax, delta);
            }
            exact += localMax == 0; aboveEight += localMax > 8;
            maximum = uint8_t(std::max(unsigned(maximum), localMax));
        }
    }
};
uint64_t number(const char *s) {
    char *end = nullptr;
    const auto value = std::strtoull(s, &end, 0);
    if (!end || *end) throw std::runtime_error("Invalid numeric option");
    return value;
}
}

int main(int argc, char **argv) try {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gs_gpu_replay PRIVATE_RECORD [--backend cpu|vulkan|both] [--from-tick N] [--until-tick N] [--max-ops N] [--dump-dir PRIVATE_DIR] [--dump-every N] [--cache-window-mib N] [--require-exact]\n");
        return 2;
    }
#ifdef PS2X_REPLAY_CPU_ONLY
    std::string backend = "cpu";
#else
    std::string backend = "both";
#endif
    std::filesystem::path dumpDir;
    uint64_t fromTick = 0, untilTick = ~uint64_t(0), maxOps = ~uint64_t(0), dumpEvery = 300;
    bool requireExact = false;
    uint64_t cacheMiB = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string option(argv[i]);
        auto argument = [&]() -> const char * {
            if (++i >= argc) throw std::runtime_error("Missing option argument");
            return argv[i];
        };
        if (option == "--backend") backend = argument();
        else if (option == "--from-tick") fromTick = number(argument());
        else if (option == "--until-tick") untilTick = number(argument());
        else if (option == "--max-ops") maxOps = number(argument());
        else if (option == "--dump-dir") dumpDir = argument();
        else if (option == "--dump-every") dumpEvery = number(argument());
        else if (option == "--cache-window-mib") cacheMiB = number(argument());
        else if (option == "--require-exact") requireExact = true;
        else throw std::runtime_error("Unknown option: " + option);
    }
    if (backend != "cpu" && backend != "vulkan" && backend != "both")
        throw std::runtime_error("Backend must be cpu, vulkan or both");
    if (cacheMiB && (cacheMiB > 2048 || !fromTick || untilTick == ~uint64_t(0) || untilTick <= fromTick || backend == "both"))
        throw std::runtime_error("Cached timing needs one backend, a nonzero bounded tick range, and at most 2048 MiB");
    const char *threads = std::getenv("PS2X_GS_THREADS");
    std::printf("configuration backend %s PS2X_GS_THREADS %s cache_limit_mib %llu\n", backend.c_str(), threads ? threads : "runtime_default", (unsigned long long)cacheMiB);
    if (!dumpDir.empty()) std::filesystem::create_directories(dumpDir);
    std::vector<Run> runs;
    auto addRun = [&](const char *name, auto &&create) {
        const auto start = Clock::now();
        Run run{name, create()};
        run.initializeSeconds = std::chrono::duration<double>(Clock::now()-start).count();
        runs.push_back(std::move(run));
    };
    if (backend != "vulkan") addRun("cpu", [] { return std::make_unique<GSCpuBackend>(); });
#ifdef PS2X_REPLAY_CPU_ONLY
    if (backend != "cpu") throw std::runtime_error("This control build includes only the CPU backend");
#else
    if (backend != "cpu") addRun("vulkan", [] { return std::make_unique<GSVulkanBackend>(); });
#endif
    if (requireExact && runs.size() != 2)
        throw std::runtime_error("Exact comparison requires --backend both");
    Reader input(argv[1]);
    uint64_t ops = 0, submits = 0, presents = 0, measuredSubmits = 0, measuredPresents = 0, lastTick = 0;
    uint64_t readbackDifferences = 0, transferDifferences = 0;
    bool initialized = false, measured = fromTick == 0, cleanEof = false;
    Difference difference;
    Clock::time_point windowStart;
    double windowSeconds = 0;
    bool windowStarted = false;
    auto each = [&](auto &&fn) { for (auto &run : runs) run.call(measured, fn); };
    auto transferCompare = [&] {
        if (runs.size() != 2) return;
        const auto a = runs[0].backend->GetTransferSnapshot(), b = runs[1].backend->GetTransferSnapshot();
        if (a.x != b.x || a.y != b.y || a.totalPixels != b.totalPixels || a.copiedPixels != b.copiedPixels || a.direction != b.direction || a.localToHostPendingBytes != b.localToHostPendingBytes)
            ++transferDifferences;
    };
    while (ops < maxOps) {
        uint8_t opcode = 0;
        if (!input.next(opcode)) { cleanEof = true; break; }
        ++ops;
        const auto op = static_cast<GSRecord::Op>(opcode);
        // GS frontend construction emits Flush, Sync(Reset), Reset before its
        // first VRAM initialization. Those valid lifecycle commands have no
        // pixel payload; reject draw/read/transfer commands before Initialize.
        if (!initialized && op != GSRecord::Op::Initialize && op != GSRecord::Op::Flush &&
            op != GSRecord::Op::Sync && op != GSRecord::Op::Reset)
            throw std::runtime_error("Pixel command precedes recording Initialize");
        switch (op) {
        case GSRecord::Op::Initialize: {
            const auto data = input.bytes(input.read<uint32_t>());
            if (data.size() != (4u << 20)) throw std::runtime_error("Only the actual 4 MiB GS VRAM ABI is supported");
            for (auto &run : runs) {
                run.vram = data;
                const auto start = Clock::now();
                run.backend->Initialize(run.vram.data(), uint32_t(run.vram.size()));
                run.initializeSeconds += std::chrono::duration<double>(Clock::now()-start).count();
            }
            initialized = true; break;
        }
        case GSRecord::Op::Reset: each([](auto &b) { b.Reset(); }); break;
        case GSRecord::Op::Submit: {
            const auto mode = input.read<int32_t>();
            const auto batch = input.read<GSPrimitiveBatch>();
            ++submits; measuredSubmits += measured;
            std::fesetround(mode);
            each([&](auto &b) { b.Submit(batch); });
            std::fesetround(FE_TONEAREST); break;
        }
        case GSRecord::Op::LoadClut: {
            const auto tex = input.read<GSTex0Reg>(); const auto clut = input.read<GSTexClutReg>();
            each([&](auto &b) { b.LoadClut(tex, clut); }); break;
        }
        case GSRecord::Op::BeginTransfer: {
            const auto command = input.read<GSTransferCommand>();
            each([&](auto &b) { b.BeginTransfer(command); }); transferCompare(); break;
        }
        case GSRecord::Op::UploadImage: {
            const auto data = input.bytes(input.read<uint32_t>());
            each([&](auto &b) { b.UploadImage(data.data(), uint32_t(data.size())); }); transferCompare(); break;
        }
        case GSRecord::Op::Flush: each([](auto &b) { b.Flush(); }); break;
        case GSRecord::Op::TextureFlush: each([](auto &b) { b.TextureFlush(); }); break;
        case GSRecord::Op::Sync: {
            const auto reason = input.read<uint8_t>();
            each([&](auto &b) { b.Sync(static_cast<GSSyncReason>(reason)); }); break;
        }
        case GSRecord::Op::Present: {
            const auto request = input.read<GSPresentationRequest>();
            ++presents; lastTick = request.vsyncTick;
            for (auto &run : runs) run.call(measured, [&](auto &b) { run.frame = b.Present(request); });
            if (lastTick % 600 == 0 && !windowStarted) {
                std::printf("progress tick %llu ops %llu submits %llu\n", (unsigned long long)lastTick, (unsigned long long)ops, (unsigned long long)submits);
                std::fflush(stdout);
            }
            if (lastTick >= fromTick) {
                measuredPresents += measured;
                if (runs.size() == 2) difference.add(runs[0].frame, runs[1].frame);
                if (!dumpDir.empty() && dumpEvery && lastTick % dumpEvery == 0 && !windowStarted) {
                    for (const auto &run : runs) {
                        char name[96]; std::snprintf(name, sizeof name, "%s_%08llu.ppm", run.name, (unsigned long long)lastTick);
                        writePpm(dumpDir / name, run.frame);
                    }
                }
                if (!measured) {
                    if (cacheMiB) {
                        input.cacheWindow(untilTick, maxOps - ops, size_t(cacheMiB) << 20);
                        std::vector<uint8_t> warmupSnapshot;
                        for (auto &run : runs) {
                            run.backend->Sync(GSSyncReason::DebugReadback);
                            run.backend->SnapshotVram(warmupSnapshot);
                        }
                        std::printf("cached_window_bytes %zu timing_starts_after_tick %llu\n", input.cache.size(), (unsigned long long)lastTick);
                        std::fflush(stdout);
                        windowStart = Clock::now(); windowStarted = true;
                    } else std::printf("timing_starts_after_tick %llu\n", (unsigned long long)lastTick);
                    measured = true;
                }
            }
            if (lastTick >= untilTick) goto finish;
            break;
        }
        case GSRecord::Op::ClearFramebuffer: {
            const auto context = input.read<GSContext>(); const auto rgba = input.read<uint32_t>();
            each([&](auto &b) { b.ClearFramebuffer(context, rgba); }); break;
        }
        case GSRecord::Op::ConsumeLocalToHost: {
            const auto size = input.read<uint32_t>();
            if (size > (64u << 20)) throw std::runtime_error("Invalid local-to-host payload size");
            std::vector<std::vector<uint8_t>> result(runs.size(), std::vector<uint8_t>(size));
            std::vector<uint32_t> sizes(runs.size());
            for (size_t i = 0; i < runs.size(); ++i) runs[i].call(measured, [&](auto &b) { sizes[i] = b.ConsumeLocalToHostBytes(result[i].data(), size); });
            if (runs.size() == 2 && (sizes[0] != sizes[1] || !std::equal(result[0].begin(), result[0].begin()+sizes[0], result[1].begin()))) ++readbackDifferences;
            break;
        }
        case GSRecord::Op::ReadVram: {
            std::array<uint32_t,5> v; for (auto &x : v) x = input.read<uint32_t>();
            uint32_t values[2]{};
            for (size_t i = 0; i < runs.size(); ++i) runs[i].call(measured, [&](auto &b) { values[i] = b.ReadVram(v[0],v[1],v[2],v[3],v[4]); });
            if (runs.size() == 2 && values[0] != values[1]) ++readbackDifferences;
            break;
        }
        case GSRecord::Op::WriteVram: {
            std::array<uint32_t,6> v; for (auto &x : v) x = input.read<uint32_t>();
            each([&](auto &b) { b.WriteVram(v[0],v[1],v[2],v[3],v[4],v[5]); }); break;
        }
        case GSRecord::Op::SnapshotVram: {
            for (auto &run : runs) { std::vector<uint8_t> snapshot; run.call(measured, [&](auto &b) { b.SnapshotVram(snapshot); }); }
            break;
        }
        default: throw std::runtime_error("Unknown operation " + std::to_string(opcode) + " at " + std::to_string(ops));
        }
    }
finish:
    if (cacheMiB && !windowStarted)
        throw std::runtime_error("Recording never reaches the cached window start");
    // A synchronized readback fences all queued GPU work. Include it in the
    // timing, rather than reporting asynchronous CPU enqueue time as GPU speed.
    for (auto &run : runs) {
        std::vector<uint8_t> snapshot;
        run.call(measured, [&](auto &b) { b.Sync(GSSyncReason::DebugReadback); b.SnapshotVram(snapshot); });
    }
    if (windowStarted) windowSeconds = std::chrono::duration<double>(Clock::now() - windowStart).count();
    for (auto &run : runs) {
        if (!dumpDir.empty()) writePpm(dumpDir / (std::string(run.name)+"_final.ppm"), run.frame);
        std::printf("backend %s initialize_s %.6f all_calls_s %.6f measured_calls_s %.6f measured_submits_per_s %.2f\n", run.name, run.initializeSeconds, run.allSeconds, run.measuredSeconds, run.measuredSeconds ? measuredSubmits / run.measuredSeconds : 0);
    }
    if (windowStarted) std::printf("cached_window_wall_s %.6f measured_submits_per_wall_s %.2f\n", windowSeconds, windowSeconds ? measuredSubmits / windowSeconds : 0);
    std::printf("ops %llu submits %llu presents %llu last_tick %llu measured_submits %llu measured_presents %llu clean_eof %u ABI_batch %zu ABI_request %zu transfer_state_differences %llu readback_differences %llu\n", (unsigned long long)ops,(unsigned long long)submits,(unsigned long long)presents,(unsigned long long)lastTick,(unsigned long long)measuredSubmits,(unsigned long long)measuredPresents,unsigned(cleanEof),sizeof(GSPrimitiveBatch),sizeof(GSPresentationRequest),(unsigned long long)transferDifferences,(unsigned long long)readbackDifferences);
    if (runs.size() == 2) std::printf("image_comparison frames %llu dimensions_differ %u pixels %llu exact_rgb_percent %.6f above8_percent %.6f mae_rgb %.6f rmse_rgb %.6f max_rgb %u\n",(unsigned long long)difference.frames,unsigned(difference.dimensionsDiffer),(unsigned long long)difference.pixels,difference.pixels ? 100.0*difference.exact/difference.pixels : 0,difference.pixels ? 100.0*difference.aboveEight/difference.pixels : 0,difference.pixels ? double(difference.absolute)/(3.0*difference.pixels) : 0,difference.pixels ? std::sqrt(double(difference.squared)/(3.0*difference.pixels)) : 0,unsigned(difference.maximum));
    if (!initialized) throw std::runtime_error("Recording is empty");
    return requireExact && (difference.dimensionsDiffer || difference.absolute || transferDifferences || readbackDifferences) ? 1 : 0;
} catch (const std::exception &error) {
    std::fprintf(stderr,"replay failed: %s\n",error.what());
    return 2;
}
