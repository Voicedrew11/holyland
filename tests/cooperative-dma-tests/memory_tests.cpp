// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>

static unsigned checks=0;
static void require(bool value, const char *text) { ++checks; if(!value) throw std::runtime_error(text); }
static void word(std::vector<uint8_t>& data, uint32_t value) { const auto p=reinterpret_cast<const uint8_t*>(&value); data.insert(data.end(),p,p+4); }
static uint32_t cmd(unsigned op,unsigned num=0,unsigned imm=0) { return op<<24|num<<16|imm; }
static void dma(PS2Memory &m, uint32_t at, const std::vector<uint8_t>& data)
{
    require(data.size()%16==0,"authored DMA aligned");
    std::memcpy(m.getRDRAM()+at,data.data(),data.size());
    m.writeIORegister(0x10009010u,at); m.writeIORegister(0x10009020u,static_cast<uint32_t>(data.size()/16));
    m.writeIORegister(0x10009000u,0x181u);
}
static std::vector<uint8_t> launch(unsigned pc=0) { std::vector<uint8_t> d; word(d,cmd(0x14,0,pc)); while(d.size()<16)word(d,0);return d; }

int main(int argc,char **argv)
{
 try {
    PS2Memory actual, reference; require(actual.initialize() && reference.initialize(),"real allocation");
    bool active=false; unsigned launches=0, cancels=0; uint32_t observedTop=0,observedItop=0;
    actual.setVu1CooperativeCallbacks([&]{return active;},[&]{active=false;++cancels;});
    actual.setVu1MscalCallback([&](uint32_t,uint32_t top,uint32_t itop){require(!active,"serialized launches");active=true;++launches;observedTop=top;observedItop=itop;});
    actual.setVu1MscntCallback([&](uint32_t,uint32_t){require(!active,"serialized resume");active=true;++launches;});

    // Independent expected values: no shared parser is used as an oracle.
    // The last V4-32 vector ends exactly at the supplied byte boundary.
    std::vector<uint8_t> boundary;word(boundary,cmd(0x6C,2,40));
    const uint32_t expected[8]={0x10203040,0x50607080,0x90A0B0C0,0xD0E0F001,
                               0x11223344,0x55667788,0x99AABBCC,0xDDEEFF00};
    for(const auto v:expected)word(boundary,v);
    actual.processVIF1Data(boundary.data(),static_cast<uint32_t>(boundary.size()));actual.serviceVif1Stream();
    for(unsigned i=0;i<8;++i){uint32_t got=0;std::memcpy(&got,actual.getVU1Data()+40*16+i*4,4);
        require(got==expected[i],"authored exact-boundary V4-32 expected value");}
    require(!actual.vif1WorkPending(),"authored exact-boundary stream fully consumed");
    if(argc>1&&std::strcmp(argv[1],"--exact-boundary")==0){std::cout<<"exact-boundary checks="<<checks<<"\n";return 0;}
    actual.cancelVif1Work();std::memset(actual.getVU1Data(),0,PS2_VU1_DATA_SIZE);
    std::memset(&actual.vif1_regs,0,sizeof(actual.vif1_regs));

    // Every split point, including partial headers, of MPG/UNPACK/mask/ROW/COL
    // must produce the synchronous state without applying partial commands.
    std::vector<uint8_t> stream;
    word(stream,cmd(1,0,0x0201)); word(stream,cmd(0x20)); word(stream,0xE4E4E4E4);
    word(stream,cmd(0x30));for(unsigned i=0;i<4;++i)word(stream,10+i);
    word(stream,cmd(0x31));for(unsigned i=0;i<4;++i)word(stream,20+i);
    word(stream,cmd(0x4A,2,0x7FF));for(unsigned i=0;i<4;++i)word(stream,0xBEEF0000+i);
    word(stream,cmd(0x7C,3,0x4005));for(unsigned i=0;i<8;++i)word(stream,100+i);
    word(stream,cmd(0x60,2,20));word(stream,7); // scalar32, fill cycle uses one source
    reference.processVIF1Data(stream.data(),static_cast<uint32_t>(stream.size()));
    for(size_t split=0;split<=stream.size();++split)
    {
        actual.cancelVif1Work(); std::memset(&actual.vif1_regs,0,sizeof(actual.vif1_regs));
        std::memset(actual.getVU1Code(),0,PS2_VU1_CODE_SIZE); std::memset(actual.getVU1Data(),0,PS2_VU1_DATA_SIZE);
        actual.processVIF1Data(stream.data(),static_cast<uint32_t>(split));actual.serviceVif1Stream();
        actual.processVIF1Data(stream.data()+split,static_cast<uint32_t>(stream.size()-split));actual.serviceVif1Stream();
        require(!actual.vif1WorkPending(),"split stream consumed");
        require(std::memcmp(&actual.vif1_regs,&reference.vif1_regs,sizeof(VIFRegisters))==0,"split registers exact");
        require(std::memcmp(actual.getVU1Code(),reference.getVU1Code(),PS2_VU1_CODE_SIZE)==0,"MPG wrap exact");
        if (std::memcmp(actual.getVU1Data(),reference.getVU1Data(),PS2_VU1_DATA_SIZE)!=0)
        {
            for(unsigned off=0;off<PS2_VU1_DATA_SIZE;++off)if(actual.getVU1Data()[off]!=reference.getVU1Data()[off]){std::cerr<<"split="<<split<<" offset="<<off<<" actual="<<unsigned(actual.getVU1Data()[off])<<" reference="<<unsigned(reference.getVU1Data()[off])<<"\n";break;}
        }
        require(std::memcmp(actual.getVU1Data(),reference.getVU1Data(),PS2_VU1_DATA_SIZE)==0,"UNPACK exact");
    }

    actual.cancelVif1Work(); std::memset(&actual.vif1_regs,0,sizeof(actual.vif1_regs));
    std::vector<uint8_t> packet;
    word(packet,cmd(3,0,16));word(packet,cmd(2,0,32));word(packet,cmd(4,0,9));word(packet,cmd(0x14));
    word(packet,cmd(0x6C,1,0));for(unsigned i=0;i<4;++i)word(packet,0x12340000+i);
    while(packet.size()%16)word(packet,0);
    dma(actual,0x1000,packet); std::memset(actual.getRDRAM()+0x1000,0,packet.size());
    for(int i=0;i<10;++i)require((actual.readIORegister(0x10009000)&0x100)!=0,"CHCR reads preserve STR");
    actual.serviceVif1Stream();require(active,"launch became active");require(observedTop==16&&observedItop==9,"launch TOP/ITOP");
    const auto tops=actual.vif1_regs.tops; const auto stat=actual.vif1_regs.stat;
    for(int i=0;i<10;++i)actual.serviceVif1Stream();
    require(actual.vif1_regs.tops==tops&&actual.vif1_regs.stat==stat,"DBF transition once");
    require((actual.readIORegister(0x10003C00)&4)!=0,"VIF VEW truthful");
    uint32_t value=0;std::memcpy(&value,actual.getVU1Data(),4);require(value!=0x12340000,"dependent UNPACK blocked");
    require(actual.consumeCompletedDmacCauses().empty(),"no early DMA cause");
    active=false;actual.serviceVif1Stream();std::memcpy(&value,actual.getVU1Data(),4);
    require(value==0x12340000,"DMA owns overwritten normal source");
    require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"one real DMA cause");
    require(!actual.vif1DmaBusy()&&(actual.readIORegister(0x10009000)&0x100)==0,"completion clears STR");
    actual.serviceVif1Stream();require(actual.consumeCompletedDmacCauses().empty(),"completion not repeated");
    actual.writeIORegister(0x1000E010u,2u); // real guest W1C acknowledgement

    // The first marker retires after its own launch, before the second launch.
    dma(actual,0x2000,launch(1));dma(actual,0x3000,launch(2));
    actual.serviceVif1Stream();require(active,"first queued launch");active=false;actual.serviceVif1Stream();
    require(active&&actual.vif1DmaBusy(),"second active keeps STR");
    require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"first queued DMA independently retires");
    actual.writeIORegister(0x1000E010u,2u);
    active=false;actual.serviceVif1Stream();require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"second DMA retires once");
    actual.writeIORegister(0x1000E010u,2u);

    // Reset cancels the epoch and source/barrier; it never fabricates completion.
    dma(actual,0x4000,launch());actual.serviceVif1Stream();const auto epoch=actual.vif1Epoch();
    actual.writeIORegister(0x10003C10,1);require(!active&&actual.vif1Epoch()>epoch,"reset cancels active epoch");
    actual.serviceVif1Stream();require(!actual.vif1WorkPending()&&actual.consumeCompletedDmacCauses().empty(),"cancel no stale completion");
    actual.writeIORegister(0x10009020,0);actual.writeIORegister(0x10009000,0x181);actual.serviceVif1Stream();
    require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"zero byte DMA exactly once");
    // A latched DMAC cause is a level, not a queue of historical completions.
    actual.writeIORegister(0x10009000,0x181);actual.serviceVif1Stream();
    require(actual.consumeCompletedDmacCauses().empty(),"unacknowledged DMAC completion coalesces");
    actual.writeIORegister(0x1000E010,2);actual.writeIORegister(0x10009000,0x181);actual.serviceVif1Stream();
    require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"W1C rearms later DMA completion");
    actual.writeIORegister(0x1000E010,2);

    // Scratchpad normal DMA snapshots wrap without borrowing the source.
    const uint32_t scratchA[4]={cmd(7,0,11),0,0,0},scratchB[4]={cmd(7,0,12),0,0,0};
    std::memcpy(actual.m_scratchpad+0x3FF0,scratchA,16);std::memcpy(actual.m_scratchpad,scratchB,16);
    actual.writeIORegister(0x10009010,0x70003FF0);actual.writeIORegister(0x10009020,2);actual.writeIORegister(0x10009000,0x181);
    std::memset(actual.m_scratchpad,0,PS2_SCRATCHPAD_SIZE);actual.serviceVif1Stream();
    require(actual.vif1_regs.mark==12,"scratchpad wrap source owned");
    require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"scratchpad DMA retires once");actual.writeIORegister(0x1000E010,2);

    // END-tag chain payload is flattened and owned at acceptance.
    const uint64_t endTag=0x70000001ull;std::memcpy(actual.getRDRAM()+0x5000,&endTag,8);
    const auto chainPayload=launch(3);std::memcpy(actual.getRDRAM()+0x5010,chainPayload.data(),16);
    actual.writeIORegister(0x10009030,0x5000);actual.writeIORegister(0x10009000,0x185);
    std::memset(actual.getRDRAM()+0x5000,0,32);actual.serviceVif1Stream();require(active,"owned chain launches after issuer source lifetime");
    active=false;actual.serviceVif1Stream();require(actual.consumeCompletedDmacCauses()==std::vector<uint32_t>{1},"chain retirement once");actual.writeIORegister(0x1000E010,2);

    // Reset inside an ordered DIRECT callback cannot invalidate parser bytes or
    // let the old command cursor leak into the fresh callback-submitted epoch.
    unsigned directCalls=0;actual.setGifPacketCallback([&](const uint8_t*,uint32_t){
        ++directCalls;actual.cancelVif1Work();const auto fresh=launch(4);actual.processVIF1Data(fresh.data(),16);
    });
    std::vector<uint8_t> direct;word(direct,cmd(0x50,0,1));word(direct,0x8000);word(direct,0x08000000);word(direct,0);word(direct,0);
    word(direct,cmd(4,0,77));actual.processVIF1Data(direct.data(),static_cast<uint32_t>(direct.size()));actual.serviceVif1Stream();
    require(directCalls==1&&!active&&actual.vif1_regs.itops!=77,"callback reset abandons old parser tail");
    actual.serviceVif1Stream();require(active,"callback fresh epoch survives old parser return");active=false;actual.serviceVif1Stream();

    // FIFO bytes are owned and do not invent a DMA completion.
    actual.setGifPacketCallback({});const auto fifo=launch(5);__m128i fifoValue=_mm_loadu_si128(reinterpret_cast<const __m128i*>(fifo.data()));
    actual.write128(0x10005000,fifoValue);actual.serviceVif1Stream();require(active&&!actual.vif1DmaBusy(),"FIFO launch separates VU from DMA busy");
    active=false;actual.serviceVif1Stream();require(actual.consumeCompletedDmacCauses().empty(),"FIFO no fictitious DMA cause");

    // Waiting for more payload is blocked work, not a continuously runnable job.
    const uint32_t partialMpg=cmd(0x4A,2,0);actual.processVIF1Data(reinterpret_cast<const uint8_t*>(&partialMpg),4);actual.serviceVif1Stream();
    require(actual.vif1WorkPending()&&!actual.vif1Runnable(),"incomplete command waits without busy dispatcher spin");actual.cancelVif1Work();

    std::cout<<"cooperative-memory checks="<<checks<<" launches="<<launches<<" cancels="<<cancels<<"\n";
    return 0;
 }catch(const std::exception &e){std::cerr<<e.what()<<" checks="<<checks<<"\n";return 1;}
}
