// SPDX-License-Identifier: GPL-3.0-only
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_vulkan_backend.h"
#include "runtime/ps2_memory.h"
#include "gs_frontend_reference.h"
#include "gs_record.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

std::unique_ptr<GSRasterBackend> makeGsRasterBackend()
{ return std::make_unique<GSCpuBackend>(); }

void require(bool condition, const char *message)
{ if (!condition) throw std::runtime_error(message); }

class CountBackend final : public GSRasterBackend
{
public:
    std::unique_ptr<GSRasterBackend> inner = std::make_unique<GSVulkanBackend>();
    std::atomic<unsigned> presents{0};
    std::function<void()> afterPresent;
    void Initialize(uint8_t *p, uint32_t s) override { inner->Initialize(p,s); }
    void Reset() override { inner->Reset(); }
    void Submit(const GSPrimitiveBatch &b) override { inner->Submit(b); }
    void LoadClut(const GSTex0Reg &t,const GSTexClutReg &c) override { inner->LoadClut(t,c); }
    void BeginTransfer(const GSTransferCommand &c) override { inner->BeginTransfer(c); }
    void UploadImage(const uint8_t *p,uint32_t n) override { inner->UploadImage(p,n); }
    void Flush() override { inner->Flush(); }
    void TextureFlush() override { inner->TextureFlush(); }
    void Sync(GSSyncReason r) override { inner->Sync(r); }
    PresentationFrame Present(const GSPresentationRequest &r) override
    {
        ++presents;
        auto result=inner->Present(r);
        if (afterPresent) afterPresent();
        return result;
    }
    bool ClearFramebuffer(const GSContext &c,uint32_t r) override {return inner->ClearFramebuffer(c,r);}
    uint32_t ConsumeLocalToHostBytes(uint8_t *p,uint32_t n) override {return inner->ConsumeLocalToHostBytes(p,n);}
    uint32_t ReadVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y) const override
    {return inner->ReadVram(p,b,w,x,y);}
    void WriteVram(uint32_t p,uint32_t b,uint32_t w,uint32_t x,uint32_t y,uint32_t v) override
    {inner->WriteVram(p,b,w,x,y,v);}
    void SnapshotVram(std::vector<uint8_t> &o) const override {inner->SnapshotVram(o);}
    GSTransferSnapshot GetTransferSnapshot() const override {return inner->GetTransferSnapshot();}
};

struct Frame
{
    std::vector<uint8_t> rgba;
    uint32_t width=0,height=0,display=0,source=0;
    bool preferred=false,present=false;
    bool operator==(const Frame &) const = default;
};
template<class G> Frame copy(G &gs)
{
    Frame frame;
    frame.present=gs.copyLatchedHostPresentationFrame(frame.rgba,frame.width,frame.height,
        &frame.display,&frame.source,&frame.preferred);
    return frame;
}

struct Pair
{
    std::vector<uint8_t> a=std::vector<uint8_t>(4u<<20),b;
    GSRegisters regs{};
    GS candidate;
    GSReference reference;
    CountBackend *ca=nullptr,*cb=nullptr;
    Pair()
    {
        for(size_t i=0;i<a.size();++i) a[i]=uint8_t((i*53u+(i>>8u))&255u);
        b=a;
        regs.pmode=1u;regs.smode2=0u;
        regs.dispfb1=1ull<<9u;
        regs.display1=(63ull<<32u)|(63ull<<44u);
        candidate.init(a.data(),uint32_t(a.size()),&regs);
        reference.init(b.data(),uint32_t(b.size()),&regs);
        auto aa=std::make_unique<CountBackend>();ca=aa.get();candidate.setRasterBackend(std::move(aa));
        auto bb=std::make_unique<CountBackend>();cb=bb.get();reference.setRasterBackend(std::move(bb));
        write(0x4c,1ull<<16u); // FRAME_1, CT32 and 64 pixels per row.
        write(0x40,(63ull<<16u)|(63ull<<48u));
        write(0x42,0u); // ALPHA_1
    }
    void write(uint8_t r,uint64_t v){candidate.writeRegister(r,v);reference.writeRegister(r,v);}
    void vr(uint32_t x,uint32_t y,uint32_t v)
    {candidate.WriteVram(GS_PSM_CT32,0,1,x,y,v);reference.WriteVram(GS_PSM_CT32,0,1,x,y,v);}
    void exact()
    {
        require(candidate.latchHostPresentationFrame(),"eligible frame was deferred");
        reference.latchHostPresentationFrame();
        require(copy(candidate)==copy(reference),"full RGBA / presentation metadata mismatch");
        std::vector<uint8_t> aa,bb;ca->SnapshotVram(aa);cb->SnapshotVram(bb);
        require(aa.size()==4u<<20 && aa==bb,"exact 4 MiB VRAM mismatch");
    }
};

void run(const std::string &name)
{
    Pair p;
    p.exact();
    if(name=="unchanged")
    {
        const auto count=p.ca->presents.load();
        for(unsigned tick:{1u,2u,57u,58u}) {p.regs.vsyncTick.store(tick);p.exact();}
        require(p.ca->presents==count,"unchanged progressive request submitted new readbacks");
    }
    else if(name=="mutation")
    {
        auto expected=p.ca->presents.load();
        p.vr(4,4,0x80112233u);p.exact();require(p.ca->presents==++expected,"same-buffer WriteVram was cached");
        p.write(0,6);p.write(1,0x3f800000802060a0ull);
        p.write(5,(2ull*16u)|((2ull*16u)<<16u));
        p.write(5,(12ull*16u)|((12ull*16u)<<16u));
        p.exact();require(p.ca->presents==++expected,"same-buffer draw was cached");
        require(p.candidate.clearFramebufferContext(0,0x80335577u),"candidate clear unsupported");
        require(p.reference.clearFramebufferContext(0,0x80335577u),"reference clear unsupported");
        p.exact();require(p.ca->presents==++expected,"same-buffer clear was cached");
        p.write(0x50,(1ull<<16u)|(1ull<<48u));
        p.write(0x51,(20ull<<32u)|(20ull<<48u));p.write(0x52,2ull|(1ull<<32u));p.write(0x53,0);
        p.write(0x54,0x8077665580112233ull);
        p.exact();require(p.ca->presents==++expected,"IMAGE/HWREG upload was cached");
        p.write(0x51,(25ull<<32u)|(25ull<<48u));p.write(0x52,1ull|(1ull<<32u));p.write(0x53,2);
        p.exact();require(p.ca->presents==++expected,"local-to-local transfer was cached");
    }
    else if(name=="movie")
    {
        p.regs.smode2=3u;p.regs.display1=(63ull<<32u)|(447ull<<44u);
        auto before=p.ca->presents.load();
        for(unsigned tick:{0u,1u,2u,3u}) {p.regs.vsyncTick.store(tick);p.exact();}
        require(p.ca->presents==before+4u,"interlaced movie phases were cached together");
    }
    else if(name=="mode")
    {
        auto before=p.ca->presents.load();
        p.regs.bgcolor=0x112233u;p.exact();
        p.regs.display1=(127ull<<32u)|(63ull<<44u);p.regs.dispfb1=2ull<<9u;p.exact();
        p.regs.pmode=2u;p.regs.dispfb2=p.regs.dispfb1;p.regs.display2=p.regs.display1;p.exact();
        require(p.ca->presents==before+3u,"mode or circuit change was cached");
    }
    else if(name=="pending")
    {
        const auto old=copy(p.candidate);const auto before=p.ca->presents.load();
        p.candidate.setHostPresentationProducerPending(true);
        p.vr(2,2,0x801010e0u);
        for(unsigned tick:{1u,2u,3u})
        {p.regs.vsyncTick.store(tick);require(!p.candidate.latchHostPresentationFrame(),"pending job admitted");}
        require(p.ca->presents==before && copy(p.candidate)==old,"pending job replaced the retained image");
        p.candidate.setHostPresentationProducerPending(false);p.exact();
        require(p.ca->presents==before+1u,"completed same-buffer job did not become visible");
    }
    else if(name=="reset")
    {
        auto before=p.candidate.hostPresentationResetEpoch();
        p.candidate.setHostPresentationProducerPending(true);p.candidate.reset();p.reference.reset();
        require(p.candidate.hostPresentationResetEpoch()!=before,"reset upload epoch did not change");
        require(!copy(p.candidate).present,"reset retained a stale host image");p.exact();
        bool called=false;
        p.ca->afterPresent=[&]{if(!called){called=true;p.candidate.reset();}};
        p.vr(5,5,0x80404040u);
        require(!p.candidate.latchHostPresentationFrame(),"pre-reset readback was published");
        require(!copy(p.candidate).present,"pre-reset image resurrected after Reset");
        p.ca->afterPresent={};p.reference.reset();p.exact();
    }
    else if(name=="concurrent")
    {
        std::mutex mutex;std::condition_variable cv;bool entered=false,release=false;
        p.vr(8,8,0x80706050u);
        p.ca->afterPresent=[&]{std::unique_lock lock(mutex);entered=true;cv.notify_one();cv.wait(lock,[&]{return release;});};
        bool admitted=true;
        std::thread host([&]{admitted=p.candidate.latchHostPresentationFrame();});
        {std::unique_lock lock(mutex);cv.wait(lock,[&]{return entered;});}
        p.candidate.reset();p.reference.reset();
        {std::lock_guard lock(mutex);release=true;}cv.notify_one();host.join();
        require(!admitted && !copy(p.candidate).present,"concurrent reset published old readback");
        p.ca->afterPresent={};p.exact();
    }
    else if(name=="replacement")
    {
        const auto before=p.candidate.hostPresentationResetEpoch();
        auto aa=std::make_unique<CountBackend>();p.ca=aa.get();p.candidate.setRasterBackend(std::move(aa));
        auto bb=std::make_unique<CountBackend>();p.cb=bb.get();p.reference.setRasterBackend(std::move(bb));
        require(p.candidate.hostPresentationResetEpoch()!=before && !copy(p.candidate).present,"replacement retained stale cache");
        p.exact();
    }
    else throw std::runtime_error("unknown case");
    std::cout<<"PASS "<<name<<": exact full RGBA/metadata and 4 MiB Vulkan VRAM; candidate readbacks="<<p.ca->presents<<"\n";
}
int main(int argc,char **argv)
{
    try{require(argc==2,"case required");run(argv[1]);return 0;}
    catch(const std::exception &error){std::cerr<<"FAIL "<<error.what()<<"\n";return 1;}
}
