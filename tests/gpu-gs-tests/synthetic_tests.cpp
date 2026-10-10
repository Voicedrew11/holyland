// SPDX-License-Identifier: GPL-3.0-only
// Generated fixtures only: this program contains no disc data or retail traces.
#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "gs_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The build supplies a small adapter for the concrete GPU backend. Keeping the
// fixture on the raster interface avoids assumptions about backend class names.
#ifndef PS2X_SYNTHETIC_CPU_ONLY
std::unique_ptr<GSRasterBackend> createGpuTestBackend();
#endif

// The standalone fixture disables optional file tracing.
namespace GSTrace
{
bool enabled() { return false; }
void imageTransfer(const GSBitBltBuf &, const GSTrxPos &, const GSTrxReg &, uint32_t) {}
}

namespace
{
constexpr uint32_t vramBytes = 4u * 1024u * 1024u;
constexpr uint32_t textureBase = 512u;
constexpr uint32_t paletteBase = 1024u;
constexpr uint32_t depthPage = 64u;
constexpr uint32_t oldColor = 0x40604020u;
constexpr uint32_t sourceColor = 0x804080c0u;
unsigned checks = 0u, failures = 0u, cases = 0u;
unsigned skippedGpuCases = 0u;

struct Result
{
    std::string label;
    uint32_t actual;
    uint32_t expected;
    uint32_t expectedGpu;

    Result(std::string label_,uint32_t actual_,uint32_t expected_) :
        label(std::move(label_)),actual(actual_),expected(expected_),expectedGpu(expected_) {}
};
using Results = std::vector<Result>;
using Case = std::function<Results(GSRasterBackend &)>;

void upload(GSRasterBackend &, uint8_t, uint32_t, uint32_t, uint32_t,
            uint16_t, uint16_t, const void *, uint32_t, uint8_t = 1u);

void expect(uint32_t actual, uint32_t expected, const std::string &label)
{
    ++checks;
    if (actual != expected)
    {
        if (failures < 32u)
            std::fprintf(stderr, "FAIL %s: got 0x%08x expected 0x%08x\n", label.c_str(), actual, expected);
        ++failures;
    }
}

void color(GSVertex &v, uint32_t rgba)
{
    v.r = static_cast<uint8_t>(rgba);
    v.g = static_cast<uint8_t>(rgba >> 8u);
    v.b = static_cast<uint8_t>(rgba >> 16u);
    v.a = static_cast<uint8_t>(rgba >> 24u);
    v.q = 1.0f;
    v.fog = 255u;
}

GSPrimitiveBatch sprite(uint32_t rgba = sourceColor, uint32_t z = 20u, uint8_t psm = GS_PSM_CT32)
{
    GSPrimitiveBatch b{};
    b.vertexCount = 2u;
    b.state.prim.type = GS_PRIM_SPRITE;
    b.state.context.frame = {0u, 1u, psm, 0u};
    b.state.context.zbuf = {depthPage, GS_PSM_Z32, false};
    b.state.context.scissor = {0u, 31u, 0u, 31u};
    b.state.context.test = 0u; // ZTE disabled, no depth writes.
    b.state.colclamp = 1u;
    b.vertices[0].x = 2.0f;
    b.vertices[0].y = 2.0f;
    b.vertices[1].x = 10.0f;
    b.vertices[1].y = 10.0f;
    for (auto &v : b.vertices)
    {
        color(v, rgba);
        v.z = z;
    }
    return b;
}

GSPrimitiveBatch triangle(bool gouraud = false)
{
    GSPrimitiveBatch b = sprite();
    b.vertexCount = 3u;
    b.state.prim.type = GS_PRIM_TRIANGLE;
    b.state.prim.iip = gouraud;
    b.vertices[0].x = 2.0f; b.vertices[0].y = 2.0f;
    b.vertices[1].x = 18.0f; b.vertices[1].y = 2.0f;
    b.vertices[2].x = 2.0f; b.vertices[2].y = 18.0f;
    return b;
}

void initializePixels(GSRasterBackend &gs, uint8_t psm = GS_PSM_CT32, uint32_t value = oldColor)
{
    gs.Reset();
    if (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S)
    {
        const std::vector<uint16_t> initial(1024u,static_cast<uint16_t>(value));
        upload(gs,psm,0u,0u,0u,32u,32u,initial.data(),static_cast<uint32_t>(initial.size()*2u));
    }
    else
    {
        const std::vector<uint32_t> initial(1024u,value);
        upload(gs,psm,0u,0u,0u,32u,32u,initial.data(),static_cast<uint32_t>(initial.size()*4u));
    }
    const std::vector<uint32_t> depth(1024u,10u);
    upload(gs,GS_PSM_Z32,depthPage*32u,0u,0u,32u,32u,depth.data(),static_cast<uint32_t>(depth.size()*4u));
    gs.TextureFlush();
}

Result pixel(GSRasterBackend &gs, const char *label, uint32_t x, uint32_t y, uint32_t expected,
             uint8_t psm = GS_PSM_CT32, uint32_t base = 0u, uint32_t bw = 1u)
{
    return {label, gs.ReadVram(psm, base, bw, x, y), expected};
}

Results drawProbe(GSRasterBackend &gs, const GSPrimitiveBatch &b, uint32_t expectedColor,
                  uint32_t expectedDepth = 10u, uint8_t framePsm = GS_PSM_CT32,
                  uint8_t depthPsm = GS_PSM_Z32)
{
    gs.Submit(b);
    gs.Sync(GSSyncReason::DebugReadback);
    return {pixel(gs, "interior color", 4u, 4u, expectedColor, framePsm),
            pixel(gs, "depth", 4u, 4u, expectedDepth, depthPsm, depthPage * 32u),
            pixel(gs, "outside primitive", 20u, 20u, framePsm == GS_PSM_CT32 ? oldColor : 0x1234u, framePsm)};
}

void texture(GSPrimitiveBatch &b, uint8_t psm = GS_PSM_CT32, bool fst = true)
{
    b.state.prim.tme = true;
    b.state.prim.fst = fst;
    b.state.context.tex0 = {textureBase, 1u, psm, 3u, 3u, 1u, 1u,
                           paletteBase, GS_PSM_CT32, 0u, 0u, 1u};
    b.state.textureWidth = 8u;
    b.state.textureHeight = 8u;
    b.vertices[0].u = 0u; b.vertices[0].v = 0u;
    b.vertices[1].u = 128u; b.vertices[1].v = 128u;
    b.state.texa = {0x40u, false, 0x80u};
}

void fillTexture(GSRasterBackend &gs, uint8_t psm, uint32_t value)
{
    if (psm == GS_PSM_T4)
    {
        const std::vector<uint8_t> data(32u,static_cast<uint8_t>((value&15u)*17u));
        upload(gs,psm,textureBase,0u,0u,8u,8u,data.data(),static_cast<uint32_t>(data.size()));
    }
    else if (psm == GS_PSM_T8)
    {
        const std::vector<uint8_t> data(64u,static_cast<uint8_t>(value));
        upload(gs,psm,textureBase,0u,0u,8u,8u,data.data(),static_cast<uint32_t>(data.size()));
    }
    else if (psm == GS_PSM_CT16)
    {
        const std::vector<uint16_t> data(64u,static_cast<uint16_t>(value));
        upload(gs,psm,textureBase,0u,0u,8u,8u,data.data(),static_cast<uint32_t>(data.size()*2u));
    }
    else
    {
        const std::vector<uint32_t> data(64u,value);
        upload(gs,psm,textureBase,0u,0u,8u,8u,data.data(),static_cast<uint32_t>(data.size()*4u));
    }
    gs.TextureFlush();
}

void upload(GSRasterBackend &gs, uint8_t psm, uint32_t base, uint32_t x, uint32_t y,
            uint16_t width, uint16_t height, const void *data, uint32_t bytes, uint8_t bufferWidth)
{
    GSTransferCommand t{};
    t.bitbltbuf.dbp = base;
    t.bitbltbuf.dbw = bufferWidth;
    t.bitbltbuf.dpsm = psm;
    t.trxpos.dsax = static_cast<uint16_t>(x);
    t.trxpos.dsay = static_cast<uint16_t>(y);
    t.trxreg = {width, height};
    t.direction = 0u;
    gs.BeginTransfer(t);
    gs.UploadImage(static_cast<const uint8_t *>(data), bytes);
}

class Fixture
{
public:
    Fixture() : cpuVram(vramBytes), gpuVram(vramBytes)
    {
#ifdef PS2X_SYNTHETIC_CPU_ONLY
        gpu = std::make_unique<GSCpuBackend>();
        std::puts("Synthetic fixture mode: two-CPU expectation control (no GPU validation)");
#else
        gpu = createGpuTestBackend();
        std::puts("Synthetic fixture mode: CPU and GPU raster backends");
#endif
        if (!gpu)
            throw std::runtime_error("GPU backend factory returned null");
        cpu.Initialize(cpuVram.data(), vramBytes);
        gpu->Initialize(gpuVram.data(), vramBytes);
    }

    void run(const char *name, const Case &test)
    {
        ++cases;
        const Results reference = test(cpu);
        const Results candidate = test(*gpu);
        expect(static_cast<uint32_t>(candidate.size()), static_cast<uint32_t>(reference.size()), std::string(name) + " result count");
        for (size_t i = 0u; i < std::min(reference.size(), candidate.size()); ++i)
        {
            const std::string label = std::string(name) + ": " + reference[i].label;
            expect(reference[i].actual, reference[i].expected, label + " CPU expected");
#ifdef PS2X_SYNTHETIC_CPU_ONLY
            const uint32_t candidateExpected=candidate[i].expected;
#else
            const uint32_t candidateExpected=candidate[i].expectedGpu;
#endif
            expect(candidate[i].actual, candidateExpected, label + " GPU expected");
            if (reference[i].expected == candidateExpected)
                expect(candidate[i].actual, reference[i].actual, label + " CPU/GPU agreement");
        }
    }

    void runGpuExpectedOnly(const char *name, const Case &test)
    {
#ifdef PS2X_SYNTHETIC_CPU_ONLY
        (void)test;
        ++skippedGpuCases;
        std::printf("SKIP GPU-specific expectation: %s\n",name);
#else
        ++cases;
        for (const auto &r:test(*gpu))
            expect(r.actual,r.expectedGpu,std::string(name)+": "+r.label+" GPU expected");
#endif
    }

    void reinitialize(GSRasterBackend &backend)
    {
        // Reuse the fixture-owned storage so the CPU backend's VRAM pointer
        // remains valid after this case. Initialize must clear command state.
        auto &storage = &backend == &cpu ? cpuVram : gpuVram;
        backend.Initialize(storage.data(), static_cast<uint32_t>(storage.size()));
    }

private:
    // Memory outlives both backends; the backend may keep the supplied pointer.
    std::vector<uint8_t> cpuVram, gpuVram;
    GSCpuBackend cpu;
    std::unique_ptr<GSRasterBackend> gpu;
};

void basicCases(Fixture &f)
{
    f.run("CT32 constant sprite", [](GSRasterBackend &gs) {
        initializePixels(gs);
        return drawProbe(gs, sprite(), sourceColor);
    });
    for (const uint8_t psm : {uint8_t(GS_PSM_CT16), uint8_t(GS_PSM_CT16S)})
        f.run(psm == GS_PSM_CT16 ? "CT16 constant sprite" : "CT16S constant sprite", [psm](GSRasterBackend &gs) {
            initializePixels(gs, psm, 0x1234u);
            return drawProbe(gs, sprite(sourceColor, 20u, psm), 0xa218u, 10u, psm);
        });
    for (bool gouraud : {false, true})
        f.run(gouraud ? "constant Gouraud triangle interior" : "constant flat triangle interior", [gouraud](GSRasterBackend &gs) {
            initializePixels(gs);
            return drawProbe(gs, triangle(gouraud), sourceColor);
        });
    f.run("Gouraud exact interior interpolation", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = triangle(true);
        color(b.vertices[0], 0x80400000u);
        color(b.vertices[1], 0x80400080u);
        color(b.vertices[2], 0x80408000u);
        Results result=drawProbe(gs,b,0x80401414u);
        // paraLLEl-GS ubershader.comp evaluates the interpolation plane at the
        // integer GS pixel position. At(4,4), the red/green weights are2/16,
        // giving16. The existing CPU rasterizer adds0.5, yielding20 instead.
        // Keep both explicit oracles; constant triangle cases compare exactly.
        result[0].expectedGpu=0x80401010u;
        return result;
    });
    f.run("inclusive scissor bounds", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = sprite(); b.state.context.scissor = {4u, 6u, 4u, 6u};
        gs.Submit(b); gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"inside",4,4,sourceColor),pixel(gs,"last included",6,6,sourceColor),
                       pixel(gs,"before scissor",3,4,oldColor),pixel(gs,"after scissor",7,4,oldColor)};
    });
    f.run("FRAME RGB channel mask", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = sprite(); b.state.context.frame.fbmsk = 0x00ff0000u;
        return drawProbe(gs, b, 0x806080c0u);
    });
    f.run("FBA forces alpha high bit", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = sprite(0x004080c0u); b.state.context.fba = 1u;
        return drawProbe(gs, b, sourceColor);
    });
    f.run("ClearFramebuffer preserves channel mask",[](GSRasterBackend &gs) {
        initializePixels(gs);auto context=sprite().state.context;
        context.frame.fbmsk=0x00ff0000u;context.scissor={2u,9u,2u,9u};
        const bool accepted=gs.ClearFramebuffer(context,sourceColor);gs.Sync(GSSyncReason::DebugReadback);
        return Results{{"accepted",uint32_t(accepted),1u},pixel(gs,"masked interior",4,4,0x806080c0u),
                       pixel(gs,"outside clear",20,20,oldColor)};
    });
}

void depthAndAlphaCases(Fixture &f)
{
    for (uint32_t method = 0u; method < 4u; ++method)
    {
        const std::string name = "disabled ZTE ignores ZTST " + std::to_string(method);
        f.run(name.c_str(), [method](GSRasterBackend &gs) {
            initializePixels(gs);
            auto b = sprite(); b.state.context.test = uint64_t(method) << 17u;
            return drawProbe(gs,b,sourceColor,10u);
        });
    }
    for (uint32_t method = 0u; method < 4u; ++method)
        for (uint32_t incoming : {10u,11u})
        {
            const std::string name = "enabled depth method " + std::to_string(method) + " incoming " + std::to_string(incoming);
            f.run(name.c_str(), [method,incoming](GSRasterBackend &gs) {
                initializePixels(gs);
                auto b = sprite(sourceColor,incoming);
                b.state.context.test = (1ull<<16u) | (uint64_t(method)<<17u);
                const bool pass = method==1u || method==2u || (method==3u && incoming>10u);
                return drawProbe(gs,b,pass?sourceColor:oldColor,pass?incoming:10u);
            });
        }
    f.run("ZMASK preserves depth", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = sprite(); b.state.context.test = 3ull<<16u; b.state.context.zbuf.zmask = true;
        return drawProbe(gs,b,sourceColor,10u);
    });
    f.run("Z16S depth GEQUAL", [](GSRasterBackend &gs) {
        initializePixels(gs);
        gs.WriteVram(GS_PSM_Z16S,depthPage*32u,1u,4u,4u,10u);
        auto b = sprite(); b.state.context.test = 5ull<<16u; b.state.context.zbuf.psm = GS_PSM_Z16S;
        return drawProbe(gs,b,sourceColor,20u,GS_PSM_CT32,GS_PSM_Z16S);
    });
    for (uint32_t failure=0u; failure<4u; ++failure)
    {
        const std::string name = "alpha NEVER failure action " + std::to_string(failure);
        f.run(name.c_str(), [failure](GSRasterBackend &gs) {
            initializePixels(gs);
            auto b = sprite(); b.state.context.test = 1ull | (uint64_t(failure)<<12u) | (3ull<<16u);
            const uint32_t expected = failure==1u?sourceColor:failure==3u?0x404080c0u:oldColor;
            return drawProbe(gs,b,expected,failure==2u?20u:10u);
        });
    }
    f.run("alpha GREATER zero rejects zero alpha", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = sprite(0x004080c0u); b.state.context.test = 1ull | (6ull<<1u);
        return drawProbe(gs,b,oldColor);
    });
    f.run("alpha GREATER zero accepts nonzero alpha", [](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b = sprite(); b.state.context.test = 1ull | (6ull<<1u);
        return drawProbe(gs,b,sourceColor);
    });
    for (uint32_t datm=0u;datm<2u;++datm)
        for (uint32_t high=0u;high<2u;++high)
        {
            const std::string name="DATE target bit "+std::to_string(datm)+" stored bit "+std::to_string(high);
            f.run(name.c_str(),[datm,high](GSRasterBackend &gs) {
                initializePixels(gs);
                const uint32_t initial = high?0x80604020u:oldColor;
                gs.WriteVram(GS_PSM_CT32,0u,1u,4u,4u,initial);
                auto b=sprite(); b.state.context.test=(1ull<<14u)|(uint64_t(datm)<<15u);
                return drawProbe(gs,b,datm==high?sourceColor:initial);
            });
        }
    const std::array<std::pair<uint64_t,uint32_t>,5> blends{{
        {0x44ull,0x40506070u}, {0x48ull,0x40808080u}, {0x42ull,0x40400000u},
        {0x09ull,0x4070a0d0u}, {0x2000000064ull,0x40585048u}}};
    for (const auto &[alpha,expected] : blends)
    {
        const std::string name="ALPHA equation "+std::to_string(alpha);
        f.run(name.c_str(),[alpha,expected](GSRasterBackend &gs) {
            initializePixels(gs);
            auto b=sprite(0x404080c0u); b.state.prim.abe=true; b.state.context.alpha=alpha;
            return drawProbe(gs,b,expected);
        });
    }
    f.run("PABE bypass for source alpha high bit clear",[](GSRasterBackend &gs) {
        initializePixels(gs);
        auto b=sprite(0x404080c0u); b.state.prim.abe=true; b.state.pabe=true; b.state.context.alpha=0x44u;
        return drawProbe(gs,b,0x404080c0u);
    });
}

void textureCases(Fixture &f)
{
    for (bool linear : {false,true})
        f.run(linear?"CT32 constant bilinear DECAL":"CT32 constant nearest DECAL",[linear](GSRasterBackend &gs) {
            initializePixels(gs); fillTexture(gs,GS_PSM_CT32,0x80804020u);
            auto b=sprite(0u); texture(b); b.state.linearFilter=linear;
            return drawProbe(gs,b,0x80804020u);
        });
    f.run("CT16 texture TEXA expansion",[](GSRasterBackend &gs) {
        initializePixels(gs); fillTexture(gs,GS_PSM_CT16,0xa218u);
        auto b=sprite(); texture(b,GS_PSM_CT16);
        return drawProbe(gs,b,sourceColor);
    });
    f.run("MODULATE neutral vertex RGB",[](GSRasterBackend &gs) {
        initializePixels(gs); fillTexture(gs,GS_PSM_CT32,0x80804020u);
        auto b=sprite(0x80808080u); texture(b); b.state.context.tex0.tfx=0u;
        return drawProbe(gs,b,0x80804020u);
    });
    f.run("STQ constant triangle interior",[](GSRasterBackend &gs) {
        initializePixels(gs); fillTexture(gs,GS_PSM_CT32,0x80804020u);
        auto b=triangle(); texture(b,GS_PSM_CT32,false);
        for(auto &v:b.vertices){v.s=0.5f;v.t=0.5f;v.q=2.0f;}
        return drawProbe(gs,b,0x80804020u);
    });
    for (const uint8_t psm : {uint8_t(GS_PSM_T4),uint8_t(GS_PSM_T8)})
        f.run(psm==GS_PSM_T4?"T4 CSM1 palette bit swap":"T8 CSM1 palette bit swap",[psm](GSRasterBackend &gs) {
            initializePixels(gs);
            const uint32_t index=psm==GS_PSM_T4?8u:16u;
            fillTexture(gs,psm,index);
            std::array<uint32_t,256> palette{};
            for(uint32_t i=0u;i<256u;++i)
            {
                const uint32_t physical=(i&~24u)|((i&8u)<<1u)|((i&16u)>>1u);
                palette[physical]=i==index?0x80804020u:0x80402080u;
            }
            upload(gs,GS_PSM_CT32,paletteBase,0u,0u,16u,16u,palette.data(),static_cast<uint32_t>(palette.size()*4u));
            gs.TextureFlush();
            auto b=sprite();texture(b,psm);
            gs.LoadClut(b.state.context.tex0,b.state.texclut);
            return drawProbe(gs,b,0x80804020u);
        });
    f.run("reversed sprite keeps UV orientation",[](GSRasterBackend &gs) {
        initializePixels(gs);
        std::array<uint32_t,64> gradient{};
        for(uint32_t y=0;y<8u;++y)for(uint32_t x=0;x<8u;++x)gradient[y*8u+x]=0x80000000u+x+1u;
        upload(gs,GS_PSM_CT32,textureBase,0u,0u,8u,8u,gradient.data(),static_cast<uint32_t>(gradient.size()*4u));
        gs.TextureFlush();
        auto b=sprite();texture(b);
        b.vertices[0].x=8;b.vertices[0].y=8;b.vertices[0].u=128;b.vertices[0].v=128;
        b.vertices[1].x=0;b.vertices[1].y=0;b.vertices[1].u=0;b.vertices[1].v=0;
        gs.Submit(b);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"left interior",2,2,0x80000003u),pixel(gs,"right interior",6,2,0x80000007u)};
    });
    f.run("fractional sprite geometry",[](GSRasterBackend &gs) {
        initializePixels(gs);fillTexture(gs,GS_PSM_CT32,0x80804020u);
        auto b=sprite();texture(b);b.vertices[0].x=0.5f;b.vertices[0].y=0.5f;
        b.vertices[1].x=8.5f;b.vertices[1].y=8.5f;
        gs.Submit(b);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"ceil excludes pixel zero",0,0,oldColor),pixel(gs,"first included",1,1,0x80804020u),
                       pixel(gs,"last included",8,8,0x80804020u),pixel(gs,"after end",9,8,oldColor)};
    });
    f.run("same-base feedback preserves constant pixels",[](GSRasterBackend &gs) {
        initializePixels(gs);
        const std::vector<uint32_t> constant(256u,0x80804020u);
        upload(gs,GS_PSM_CT32,0u,0u,0u,16u,16u,constant.data(),static_cast<uint32_t>(constant.size()*4u));
        auto b=sprite();texture(b);b.state.context.tex0.tbp0=0u;b.state.context.tex0.tw=4u;b.state.context.tex0.th=4u;
        b.state.textureWidth=16u;b.state.textureHeight=16u;
        b.vertices[0].x=0;b.vertices[0].y=0;b.vertices[1].x=8;b.vertices[1].y=8;
        gs.TextureFlush();gs.Submit(b);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"feedback interior",4,4,0x80804020u),pixel(gs,"untouched source",12,12,0x80804020u)};
    });
}

void gpuSpecificCases(Fixture &f)
{
    // CPU SampleTexture currently ignores mipmapping and WritePixel ignores
    // SCANMSK. These cases assert hardware behavior without treating CPU output
    // as an oracle for features it does not implement.
    f.runGpuExpectedOnly("MTBA automatic level1 address and fixed LOD",[](GSRasterBackend &gs) {
        initializePixels(gs);fillTexture(gs,GS_PSM_CT32,0x80000040u);
        const std::vector<uint32_t> level1(16u,0x80004000u);
        // 8x8 CT32 occupies one 256-byte block. MTBA places level1 at TBP0+1.
        upload(gs,GS_PSM_CT32,textureBase+1u,0u,0u,4u,4u,level1.data(),64u);
        gs.TextureFlush();auto b=sprite();texture(b);
        b.state.context.tex1=(16ull<<32u)|1ull|(1ull<<2u)|(2ull<<6u)|(1ull<<9u);
        gs.Submit(b);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"green level1 selected",4,4,0x80004000u)};
    });
    for(uint32_t mask:{2u,3u})
    {
        const std::string name="SCANMSK "+std::to_string(mask);
        f.runGpuExpectedOnly(name.c_str(),[mask](GSRasterBackend &gs) {
            initializePixels(gs);auto b=sprite();b.state.scanmsk=mask;
            gs.Submit(b);gs.Sync(GSSyncReason::DebugReadback);
            return Results{pixel(gs,"even row",4,4,mask==2u?oldColor:sourceColor),
                           pixel(gs,"odd row",4,5,mask==3u?oldColor:sourceColor)};
        });
    }
}

void transferCases(Fixture &f)
{
    f.run("HOST2LOCAL CT32 upload and progress",[](GSRasterBackend &gs) {
        initializePixels(gs);
        const std::array<uint32_t,4> data{{0x80010203u,0x80040506u,0x80070809u,0x800a0b0cu}};
        upload(gs,GS_PSM_CT32,0u,3u,4u,2u,2u,data.data(),8u);
        const auto before=gs.GetTransferSnapshot();
        gs.UploadImage(reinterpret_cast<const uint8_t *>(data.data()+2),8u);
        const auto after=gs.GetTransferSnapshot();gs.Sync(GSSyncReason::DebugReadback);
        return Results{{"partial copied pixels",before.copiedPixels,2u},{"complete copied pixels",after.copiedPixels,4u},
            pixel(gs,"first",3,4,data[0]),pixel(gs,"second",4,4,data[1]),
            pixel(gs,"third",3,5,data[2]),pixel(gs,"fourth",4,5,data[3])};
    });
    f.run("HOST2LOCAL CT16 upload",[](GSRasterBackend &gs) {
        initializePixels(gs);
        const std::array<uint16_t,4> data{{0x8001u,0x8400u,0x83e0u,0xffffu}};
        upload(gs,GS_PSM_CT16,textureBase,3u,4u,2u,2u,data.data(),8u);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"first",3,4,data[0],GS_PSM_CT16,textureBase),pixel(gs,"last",4,5,data[3],GS_PSM_CT16,textureBase)};
    });
    f.run("HOST2LOCAL packed T4 nibbles",[](GSRasterBackend &gs) {
        initializePixels(gs);const uint8_t data=0x21u;
        upload(gs,GS_PSM_T4,textureBase,0u,0u,2u,1u,&data,1u);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"low nibble",0,0,1u,GS_PSM_T4,textureBase),pixel(gs,"high nibble",1,0,2u,GS_PSM_T4,textureBase)};
    });
    f.run("LOCAL2LOCAL copy",[](GSRasterBackend &gs) {
        initializePixels(gs);gs.WriteVram(GS_PSM_CT32,0u,1u,3u,4u,sourceColor);
        GSTransferCommand t{};t.bitbltbuf={0u,1u,GS_PSM_CT32,textureBase,1u,GS_PSM_CT32};
        t.trxpos={3u,4u,5u,6u,0u};t.trxreg={1u,1u};t.direction=2u;
        gs.BeginTransfer(t);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"source preserved",3,4,sourceColor),pixel(gs,"destination",5,6,sourceColor,GS_PSM_CT32,textureBase)};
    });
    f.run("LOCAL2HOST null-safe and chunked readback",[](GSRasterBackend &gs) {
        initializePixels(gs);gs.WriteVram(GS_PSM_CT32,0u,1u,3u,4u,sourceColor);
        GSTransferCommand t{};t.bitbltbuf={0u,1u,GS_PSM_CT32,0u,1u,GS_PSM_CT32};
        t.trxpos.ssax=3u;t.trxpos.ssay=4u;t.trxreg={1u,1u};t.direction=1u;gs.BeginTransfer(t);
        std::array<uint8_t,4> bytes{};
        const uint32_t nullRead=gs.ConsumeLocalToHostBytes(nullptr,4u);
        const auto pendingAfterNull=gs.GetTransferSnapshot();
        const uint32_t first=gs.ConsumeLocalToHostBytes(bytes.data(),2u);
        const uint32_t second=gs.ConsumeLocalToHostBytes(bytes.data()+2,2u);
        const uint32_t empty=gs.ConsumeLocalToHostBytes(bytes.data(),0u);
        const uint32_t packed=uint32_t(bytes[0])|(uint32_t(bytes[1])<<8u)|(uint32_t(bytes[2])<<16u)|(uint32_t(bytes[3])<<24u);
        return Results{{"null destination returns zero",nullRead,0u},
            {"null destination preserves pending bytes",static_cast<uint32_t>(pendingAfterNull.localToHostPendingBytes),4u},
            {"first chunk",first,2u},{"second chunk",second,2u},{"zero request",empty,0u},{"readback color",packed,sourceColor}};
    });
    for (bool queuedReadback : {false,true})
        f.run(queuedReadback ? "repeated Initialize clears queued readback" : "repeated Initialize clears partial upload",
              [&f,queuedReadback](GSRasterBackend &gs) {
            initializePixels(gs);
            const std::array<uint32_t,2> colors{{0x80010203u,0x80040506u}};
            Results result;
            if (queuedReadback)
            {
                upload(gs,GS_PSM_CT32,0u,4u,4u,2u,1u,colors.data(),8u);
                GSTransferCommand command{};
                command.bitbltbuf={0u,1u,GS_PSM_CT32,0u,1u,GS_PSM_CT32};
                command.trxpos.ssax=4u;command.trxpos.ssay=4u;
                command.trxreg={2u,1u};command.direction=1u;
                gs.BeginTransfer(command);
                result.emplace_back("readback initially queued",static_cast<uint32_t>(gs.GetTransferSnapshot().localToHostPendingBytes),8u);
            }
            else
            {
                upload(gs,GS_PSM_CT32,0u,4u,4u,2u,1u,colors.data(),1u);
                const auto partial=gs.GetTransferSnapshot();
                result.emplace_back("partial upload remains active",partial.direction,0u);
                result.emplace_back("partial upload has no complete pixels",partial.copiedPixels,0u);
            }
            f.reinitialize(gs);
            const auto cleared=gs.GetTransferSnapshot();
            std::array<uint8_t,8> discarded{};
            result.emplace_back("Initialize clears direction",cleared.direction,3u);
            result.emplace_back("Initialize clears copied pixels",cleared.copiedPixels,0u);
            result.emplace_back("Initialize clears total pixels",cleared.totalPixels,0u);
            result.emplace_back("Initialize clears pending bytes",static_cast<uint32_t>(cleared.localToHostPendingBytes),0u);
            result.emplace_back("Initialize clears queued FIFO",gs.ConsumeLocalToHostBytes(discarded.data(),8u),0u);
            // With no transfer active, leftover input must be ignored.
            gs.UploadImage(reinterpret_cast<const uint8_t *>(colors.data()),8u);
            result.emplace_back("unrequested upload leaves idle state",gs.GetTransferSnapshot().direction,3u);
            upload(gs,GS_PSM_CT32,0u,7u,6u,2u,1u,colors.data(),8u);
            result.emplace_back("fresh transfer copies only its pixels",gs.GetTransferSnapshot().copiedPixels,2u);
            gs.Sync(GSSyncReason::DebugReadback);
            result.push_back(pixel(gs,"fresh transfer first pixel",7u,6u,colors[0]));
            result.push_back(pixel(gs,"fresh transfer last pixel",8u,6u,colors[1]));
            return result;
        });
    f.run("upload TEXFLUSH visibility",[](GSRasterBackend &gs) {
        initializePixels(gs);fillTexture(gs,GS_PSM_CT32,0x80000040u);
        auto first=sprite();texture(first);gs.Submit(first);
        const std::vector<uint32_t> green(64u,0x80004000u);
        upload(gs,GS_PSM_CT32,textureBase,0u,0u,8u,8u,green.data(),static_cast<uint32_t>(green.size()*4u));
        gs.TextureFlush();auto second=first;second.vertices[0].x+=12.0f;second.vertices[1].x+=12.0f;
        gs.Submit(second);gs.Sync(GSSyncReason::DebugReadback);
        return Results{pixel(gs,"before upload",4,4,0x80000040u),pixel(gs,"after flush",16,4,0x80004000u)};
    });
    for(uint16_t width:{uint16_t(1u),uint16_t(3u),uint16_t(5u)})
    {
        const std::string name="HOST2LOCAL CT32 short/tail width "+std::to_string(width);
        f.run(name.c_str(),[width](GSRasterBackend &gs) {
            initializePixels(gs);std::vector<uint32_t> data(width);
            for(uint32_t i=0;i<width;++i)data[i]=0x80000011u+i;
            upload(gs,GS_PSM_CT32,0u,4u,4u,width,1u,data.data(),uint32_t(data.size()*4u));
            gs.Sync(GSSyncReason::DebugReadback);Results result;
            for(uint32_t i=0;i<width;++i)result.push_back(pixel(gs,"uploaded pixel",4u+i,4u,data[i]));
            result.push_back(pixel(gs,"padding does not overwrite next pixel",4u+width,4u,oldColor));
            return result;
        });
    }
    for(uint16_t width:{uint16_t(3u),uint16_t(18u)})
    {
        const std::string name="HOST2LOCAL T4 short/tail width "+std::to_string(width);
        f.run(name.c_str(),[width](GSRasterBackend &gs) {
            initializePixels(gs);std::vector<uint8_t> data((width+1u)/2u,0u);
            for(uint32_t i=0;i<width;++i)data[i/2u]|=uint8_t(((i%15u)+1u)<<((i&1u)*4u));
            gs.WriteVram(GS_PSM_T4,textureBase,1u,width,0u,7u);
            upload(gs,GS_PSM_T4,textureBase,0u,0u,width,1u,data.data(),uint32_t(data.size()));
            gs.Sync(GSSyncReason::DebugReadback);Results result;
            for(uint32_t i=0;i<width;++i)result.push_back(pixel(gs,"uploaded nibble",i,0u,(i%15u)+1u,GS_PSM_T4,textureBase));
            result.push_back(pixel(gs,"padding nibble preserved",width,0u,7u,GS_PSM_T4,textureBase));
            return result;
        });
    }
    for(uint16_t width:{uint16_t(3u),uint16_t(5u)})
    {
        const std::string name="LOCAL2HOST CT32 partial128bit tail width "+std::to_string(width);
        f.run(name.c_str(),[width](GSRasterBackend &gs) {
            initializePixels(gs);
            for(uint32_t i=0;i<width;++i)gs.WriteVram(GS_PSM_CT32,0u,1u,4u+i,4u,0x80000011u+i);
            GSTransferCommand t{};t.bitbltbuf={0u,1u,GS_PSM_CT32,0u,1u,GS_PSM_CT32};
            t.trxpos.ssax=4u;t.trxpos.ssay=4u;t.trxreg={width,1u};t.direction=1u;gs.BeginTransfer(t);
            std::vector<uint8_t> bytes(width*4u);
            const uint32_t first=gs.ConsumeLocalToHostBytes(bytes.data(),2u);
            const uint32_t tail=gs.ConsumeLocalToHostBytes(bytes.data()+2u,uint32_t(bytes.size()-2u));
            const uint32_t empty=gs.ConsumeLocalToHostBytes(bytes.data(),1u);
            Results result{{"first bytes",first,2u},{"all tail bytes",tail,uint32_t(bytes.size()-2u)},{"FIFO exhausted",empty,0u}};
            for(uint32_t i=0;i<width;++i)
            {
                const uint32_t offset=i*4u;
                const uint32_t packed=uint32_t(bytes[offset])|(uint32_t(bytes[offset+1u])<<8u)|
                    (uint32_t(bytes[offset+2u])<<16u)|(uint32_t(bytes[offset+3u])<<24u);
                result.emplace_back("readback pixel",packed,0x80000011u+i);
            }
            return result;
        });
    }
}

GSPresentationRequest fullSizePresentation()
{
    GSPresentationRequest request{};
    // Real 640x448 CRT2 frame-mode timing: exercise the supported GPU scanout
    // path, rather than its small-fixture CPU presentation fallback.
    request.pmode = 2u;
    request.smode2 = 3u;
    request.dispfb2 = 10ull << 9u;
    request.display2 = 636ull | (50ull << 12u) | (3ull << 23u) |
                       (2559ull << 32u) | (895ull << 44u);
    return request;
}

uint32_t presentedPixel(const PresentationFrame &frame, uint32_t x, uint32_t y)
{
    const size_t offset = (static_cast<size_t>(y) * 640u + x) * 4u;
    if (offset + 3u >= frame.pixels.size())
        throw std::runtime_error("Cross-thread presentation returned a truncated host frame");
    const auto *p = frame.pixels.data() + offset;
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8u) |
           (uint32_t(p[2]) << 16u) | (uint32_t(p[3]) << 24u);
}

Results presentationResults(const PresentationFrame &frame)
{
    return {{"main-thread presentation is valid", static_cast<uint32_t>(bool(frame)), 1u},
            {"logical width", frame.width, 640u}, {"logical height", frame.height, 448u},
            {"host buffer uses fixed640 row stride", static_cast<uint32_t>(frame.pixels.size()), 640u*512u*4u}};
}

void crossThreadCases(Fixture &f)
{
    f.run("worker upload and draw then main-thread scanout/readback", [](GSRasterBackend &gs) {
        // Fixture construction and Initialize run on main. Move the command
        // producer to another thread, then hand presentation back to main.
        gs.Reset();
        const std::vector<uint32_t> background(640u*448u, oldColor);
        const uint32_t workerTextureBase = 8192u; // 2MiB, outside the scene framebuffer.
        constexpr uint32_t workerColor = 0x80804020u;
        const std::vector<uint32_t> texels(64u, workerColor);
        std::exception_ptr workerError;
        std::jthread worker([&] {
            try
            {
                upload(gs, GS_PSM_CT32, 0u, 0u, 0u, 640u, 448u, background.data(),
                       static_cast<uint32_t>(background.size()*4u), 10u);
                upload(gs, GS_PSM_CT32, workerTextureBase, 0u, 0u, 8u, 8u, texels.data(), 256u);
                gs.TextureFlush();
                auto batch = sprite();
                texture(batch);
                batch.state.context.frame.fbw = 10u;
                batch.state.context.scissor = {0u, 639u, 0u, 447u};
                batch.state.context.zbuf.zmask = true;
                batch.state.context.tex0.tbp0 = workerTextureBase;
                gs.Submit(batch);
                gs.Flush();
            }
            catch (...) { workerError = std::current_exception(); }
        });
        worker.join();
        if (workerError) std::rethrow_exception(workerError);
        const auto frame = gs.Present(fullSizePresentation());
        Results result = presentationResults(frame);
        result.emplace_back("scanout uploaded/textured RGB with alpha255", presentedPixel(frame,4u,4u), workerColor|0xff000000u);
        result.emplace_back("scanout untouched background", presentedPixel(frame,20u,20u), oldColor|0xff000000u);
        result.push_back(pixel(gs,"main readback uploaded/textured draw",4u,4u,workerColor,GS_PSM_CT32,0u,10u));
        result.push_back(pixel(gs,"main readback untouched background",20u,20u,oldColor,GS_PSM_CT32,0u,10u));
        return result;
    });
    f.run("concurrent caller threads submit disjoint sprites", [](GSRasterBackend &gs) {
        gs.Reset();
        const std::vector<uint32_t> background(640u*448u, oldColor);
        upload(gs, GS_PSM_CT32, 0u, 0u, 0u, 640u, 448u, background.data(),
               static_cast<uint32_t>(background.size()*4u), 10u);
        gs.TextureFlush();
        constexpr std::array<uint32_t,4> colors{{0x80000040u,0x80004000u,0x80400000u,0x80604020u}};
        std::array<std::jthread,4> callers;
        std::array<std::exception_ptr,4> errors{};
        std::atomic<bool> start{false};
        try
        {
            for (size_t i=0u; i<callers.size(); ++i)
                callers[i] = std::jthread([&,i] {
                    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                    try
                    {
                        auto batch = sprite(colors[i]);
                        batch.state.context.frame.fbw = 10u;
                        batch.state.context.scissor = {0u,639u,0u,447u};
                        batch.state.context.zbuf.zmask = true;
                        const float x = float((i&1u)*12u), y = float((i>>1u)*12u);
                        for (auto &v:batch.vertices) { v.x+=x; v.y+=y; }
                        // Repeated submissions overlap only within this caller;
                        // distinct callers never compete for a destination pixel.
                        for (unsigned repeat=0u; repeat<8u; ++repeat) gs.Submit(batch);
                        gs.Flush();
                    }
                    catch (...) { errors[i] = std::current_exception(); }
                });
        }
        catch (...)
        {
            start.store(true,std::memory_order_release);
            for (auto &caller:callers) if (caller.joinable()) caller.join();
            throw;
        }
        start.store(true,std::memory_order_release);
        for (auto &caller:callers) caller.join();
        for (const auto &error:errors) if (error) std::rethrow_exception(error);
        const auto frame = gs.Present(fullSizePresentation());
        Results result = presentationResults(frame);
        for (size_t i=0u; i<colors.size(); ++i)
        {
            const uint32_t x=4u+uint32_t(i&1u)*12u, y=4u+uint32_t(i>>1u)*12u;
            result.emplace_back("each caller survives GPU scanout",presentedPixel(frame,x,y),colors[i]|0xff000000u);
            result.push_back(pixel(gs,"each caller survives main readback",x,y,colors[i],GS_PSM_CT32,0u,10u));
        }
        result.emplace_back("scanout excludes outside pixels",presentedPixel(frame,27u,27u),oldColor|0xff000000u);
        result.push_back(pixel(gs,"readback excludes outside pixels",27u,27u,oldColor,GS_PSM_CT32,0u,10u));
        return result;
    });
}
} // namespace

int main()
{
    try
    {
        Fixture fixture;
        basicCases(fixture);
        depthAndAlphaCases(fixture);
        textureCases(fixture);
        transferCases(fixture);
        crossThreadCases(fixture);
        gpuSpecificCases(fixture);
        std::printf("%u cases, %u checks, %u failures, %u GPU-specific cases skipped\n",cases,checks,failures,skippedGpuCases);
        return failures==0u?0:1;
    }
    catch(const std::exception &error)
    {
        std::fprintf(stderr,"Fixture could not run: %s\n",error.what());
        return 2;
    }
}
