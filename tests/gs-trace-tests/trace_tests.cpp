// SPDX-License-Identifier: GPL-3.0-only
#include "gs_trace.h"
#include "runtime/ps2_memory.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
void expect(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void environment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}
std::string read(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
unsigned count(const std::string& text, const std::string& token) {
    unsigned result = 0;
    size_t pos = 0;
    while ((pos = text.find(token, pos)) != std::string::npos) { ++result; pos += token.size(); }
    return result;
}
GSPrimitiveBatch primitive() {
    GSPrimitiveBatch batch;
    batch.vertexCount = 2;
    batch.state.prim.type = GS_PRIM_SPRITE;
    batch.state.context.xyoffset.ofx = 32776; //2048.5
    batch.state.context.xyoffset.ofy = 32788; //2049.25
    batch.vertices[0].x = 2049.25f;
    batch.vertices[0].y = 2050.875f;
    batch.vertices[1].x = 2051.5f;
    batch.vertices[1].y = 2052.75f;
    return batch;
}
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string mode = argv[1], stem = argv[2], trace = stem + ".txt", dump = stem + ".vram";
    const bool disabled = mode == "disabled", vramOnly = mode == "vram_only";
    environment("PS2X_GS_TRACE", disabled || vramOnly ? "" : trace);
    environment("PS2X_GS_TRACE_FROM", "10");
    environment("PS2X_GS_TRACE_TO", "11");
    environment("PS2X_VRAM_DUMP", disabled ? "" : dump);

    GSRegisters registers{};
    registers.pmode = 2;
    registers.smode2 = 3;
    registers.dispfb1 = 8 | (5ull << 9);
    registers.display1 = (319ull << 32) | (223ull << 44);
    registers.dispfb2 = 70 | (10ull << 9) | (2ull << 15) | (7ull << 32) | (3ull << 43);
    registers.display2 = 636 | (32ull << 12) | (1ull << 23) | (2ull << 27) |
                         (1279ull << 32) | (1535ull << 44);
    unsigned snapshotCalls = 0;
    GSTrace::setPrivilegedRegisters(&registers);
    GSTrace::setSnapshotCallback([&](std::vector<uint8_t>& data) { ++snapshotCalls; data = {1,2,3,4,5,6,7,8}; });
    GSTrace::initialize();
    expect(GSTrace::enabled() == !disabled, "enabled flag follows trace/dump configuration");

    GSBitBltBuf transfer{};
    transfer.dbp = 40;
    transfer.dbw = 1;
    transfer.dpsm = GS_PSM_CT32;
    GSTrxPos origin{};
    GSTrxReg dimensions{2, 3};
    const auto batch = primitive();
    if (mode == "crt2") {
        GSTrace::onTick(10);
        GSTrace::onTick(10);
    } else if (mode == "fraction") {
        GSTrace::drawPrimitive(batch, 10);
    } else {
        for (uint64_t tick = 9; tick <= 12; ++tick) {
            GSTrace::setTick(tick);
            if (mode != "register_only" && mode != "transfer_only" && mode != "draw_only") GSTrace::onTick(tick);
            if (mode != "transfer_only" && mode != "draw_only") GSTrace::registerWrite(GS_REG_PRIM, 6, tick);
            if (mode != "register_only" && mode != "draw_only") GSTrace::imageTransfer(transfer, origin, dimensions, 0);
            if (mode != "register_only" && mode != "transfer_only") GSTrace::drawPrimitive(batch, tick);
        }
        // Crossing TO closes recording even if a later erroneous tick is smaller.
        GSTrace::registerWrite(GS_REG_PRIM, 6, 10);
    }
    GSTrace::shutdown();
    GSTrace::shutdown();
    const std::string output = read(trace), vram = read(dump);
    if (disabled) {
        expect(output.empty() && vram.empty() && snapshotCalls == 0, "disabled diagnostics produce no files or snapshots");
    } else {
        expect(vram == std::string("\1\2\3\4\5\6\7\10", 8), "shutdown preserves snapshot callback VRAM dump");
        expect(snapshotCalls == 1, "shutdown is idempotent and dumps once");
        if (vramOnly) expect(output.empty(), "VRAM-only diagnostics do not create trace records");
        else if (mode == "crt2") {
            expect(count(output, "PRIV tick=10") == 1, "duplicate onTick emits privileged state once");
            expect(output.find("DISPFB1(fbp=8 fbw=5 psm=0x0 dbx=0 dby=0)") != std::string::npos, "CRT1 state remains present");
            expect(output.find("DISPFB2(fbp=70 fbw=10 psm=0x2 dbx=7 dby=3)") != std::string::npos, "CRT2 framebuffer fields are distinct and complete");
            expect(output.find("DISPLAY2(dx=636 dy=32 magh=1 magv=2 dw=1279 dh=1535)") != std::string::npos, "CRT2 display fields are distinct and complete");
        } else if (mode == "fraction") {
            expect(output.find("xy=[0.75,3]x[1.625,3.5]") != std::string::npos, "draw bounds preserve fractional XYOFFSET");
            expect(output.find("xy=(0.75,1.625)") != std::string::npos, "per-vertex trace preserves fractional XYOFFSET");
        } else {
            expect(output.find("tick=9 ") == std::string::npos && output.find("tick 9 ") == std::string::npos, "FROM excludes all earlier records");
            expect(output.find("tick=12 ") == std::string::npos && output.find("tick 12 ") == std::string::npos, "TO excludes all later records");
            expect(count(output, "=== VSYNC tick ") == 2, "inclusive FROM/TO emit exactly two tick headers");
            if (mode != "transfer_only" && mode != "draw_only") expect(count(output, "REG tick=") == 2, "register records honor inclusive range and stop after TO");
            if (mode != "register_only" && mode != "draw_only") expect(count(output, "XFER tick=") == 2, "transfer records honor inclusive range");
            if (mode != "register_only" && mode != "transfer_only") expect(count(output, "DRAW tick=") == 2, "draw records honor inclusive range");
            if (mode != "register_only" && mode != "transfer_only" && mode != "draw_only") expect(count(output, "PRIV tick=") == 2, "privileged snapshots honor inclusive range");
        }
    }
    std::printf("%s: %u checks, %u failures\n", mode.c_str(), checks, failures);
    return failures ? 1 : 0;
}
