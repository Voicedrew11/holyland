// SPDX-License-Identifier: GPL-3.0-only
#include "ps2_runtime.h"
#include "runtime/gs/gs_cpu_backend.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void require(bool value, const char *text)
{ ++checks; if (!value) throw std::runtime_error(text); }

class CountBackend final : public GSRasterBackend
{
    GSCpuBackend inner;
public:
    unsigned presents = 0;
    void Initialize(uint8_t*p,uint32_t n) override {inner.Initialize(p,n);}
    void Reset() override {inner.Reset();}
    void Submit(const GSPrimitiveBatch&v) override {inner.Submit(v);}
    void LoadClut(const GSTex0Reg&a,const GSTexClutReg&b) override {inner.LoadClut(a,b);}
    void BeginTransfer(const GSTransferCommand&v) override {inner.BeginTransfer(v);}
    void UploadImage(const uint8_t*p,uint32_t n) override {inner.UploadImage(p,n);}
    void Flush() override {inner.Flush();}
    void TextureFlush() override {inner.TextureFlush();}
    void Sync(GSSyncReason v) override {inner.Sync(v);}
    PresentationFrame Present(const GSPresentationRequest&v) override {++presents;return inner.Present(v);}
    bool ClearFramebuffer(const GSContext&v,uint32_t c) override {return inner.ClearFramebuffer(v,c);}
    uint32_t ConsumeLocalToHostBytes(uint8_t*p,uint32_t n) override {return inner.ConsumeLocalToHostBytes(p,n);}
    uint32_t ReadVram(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e)const override {return inner.ReadVram(a,b,c,d,e);}
    void WriteVram(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f)override {inner.WriteVram(a,b,c,d,e,f);}
    void SnapshotVram(std::vector<uint8_t>&v)const override {inner.SnapshotVram(v);}
    GSTransferSnapshot GetTransferSnapshot()const override {return inner.GetTransferSnapshot();}
};
struct Frame {
    std::vector<uint8_t> pixels;
    uint32_t width=0,height=0,display=0,source=0;
    bool preferred=false,valid=false;
    bool operator==(const Frame&)const=default;
};
Frame copy(GS&gs) {
    Frame result;
    result.valid=gs.copyLatchedHostPresentationFrame(result.pixels,result.width,result.height,
        &result.display,&result.source,&result.preferred);
    return result;
}
void run(const std::string&mode) {
    auto runtime=std::make_unique<PS2Runtime>();
    require(runtime->memory().initialize(),"actual PS2Memory initializes");
    require(runtime->syncCoreSubsystems(),"actual cooperative bindings initialize");
    auto&memory=runtime->memory();auto&gs=runtime->gs();
    auto backend=std::make_unique<CountBackend>();auto*count=backend.get();
    gs.setRasterBackend(std::move(backend));
    memory.gs().pmode=1;memory.gs().smode2=0;memory.gs().dispfb1=1ull<<9;
    memory.gs().display1=(63ull<<32)|(63ull<<44);
    gs.writeRegister(0x4c,1ull<<16);
    gs.WriteVram(GS_PSM_CT32,0,1,2,2,0x80112233);
    require(gs.latchHostPresentationFrame(),"initial completed frame admits");
    const Frame old=copy(gs);const unsigned before=count->presents;
    require(old.valid,"initial image is available");
    if(mode=="blocked") {
        const uint32_t unpack=0x6c010000;
        memory.processVIF1Data(reinterpret_cast<const uint8_t*>(&unpack),4);
        runtime->serviceCooperativeVif1(1);
        require(memory.vif1WorkPending()&&!memory.vif1Runnable(),"partial input blocks real VIF cursor");
        gs.WriteVram(GS_PSM_CT32,0,1,2,2,0x80445566);
        require(gs.latchHostPresentationFrame(),"input-blocked stream permits presentation");
        require(copy(gs)!=old&&count->presents==before+1,"input-blocked mutation becomes visible");
        const uint32_t payload[4]={1,2,3,4};
        memory.processVIF1Data(reinterpret_cast<const uint8_t*>(payload),16);
        runtime->serviceCooperativeVif1(1);
        require(!memory.vif1WorkPending(),"queued payload completes real VIF stream");
        require(gs.latchHostPresentationFrame(),"completed payload remains eligible");
        return;
    }
    // An authored 34-pair program remains active across one-cycle quanta.
    for(unsigned index=0;index<34;++index)
        memory.write64(PS2_VU1_CODE_BASE+index*8,uint64_t(index==32?0x400002ffu:0x2ffu)<<32);
    const uint32_t launch[4]={0x14000000,0,0,0};
    memory.processVIF1Data(reinterpret_cast<const uint8_t*>(launch),16);
    runtime->serviceCooperativeVif1(1);
    require(runtime->vu1().jobActive()&&memory.vif1Runnable(),"actual launch leaves bounded VU job active");
    gs.WriteVram(GS_PSM_CT32,0,1,2,2,0x80778899);
    for(unsigned tick=1;tick<4;++tick) {
        memory.gs().vsyncTick.store(tick);
        require(!gs.latchHostPresentationFrame(),"active actual producer defers every host request");
        require(copy(gs)==old&&count->presents==before,"deferred request retains full old pixels and metadata");
    }
    if(mode=="cancel"||mode=="reset") {
        const auto epoch=gs.hostPresentationResetEpoch();
        memory.cancelVif1Work();
        require(!runtime->vu1().jobActive()&&!memory.vif1WorkPending(),"real cancellation clears job and stream");
        if(mode=="reset") {
            gs.reset();
            require(gs.hostPresentationResetEpoch()!=epoch&&!copy(gs).valid,"reset invalidates actual retained frame");
        }
    } else {
        require(mode=="normal","known fixture mode");
        for(unsigned n=0;n<200&&(runtime->vu1().jobActive()||memory.vif1WorkPending());++n)
            runtime->serviceCooperativeVif1(1);
        require(!runtime->vu1().jobActive()&&!memory.vif1WorkPending(),"bounded actual producer completes");
    }
    require(gs.latchHostPresentationFrame(),"completion or cancellation clears admission gate");
    require(copy(gs).valid&&count->presents==before+1,"one completed image is submitted after gate release");
    if(mode!="reset")require(copy(gs)!=old,"same-buffer mutation is visible after completion");
}
}
int main(int argc,char**argv) {
    try {require(argc==2,"mode required");run(argv[1]);std::cout<<"PASS runtime admission "<<argv[1]<<" checks="<<checks<<'\n';return 0;}
    catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
}
