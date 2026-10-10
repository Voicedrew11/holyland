// SPDX-License-Identifier: GPL-3.0-or-later
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "iop_kernel_reference.h"
#if defined(CANDIDATE_KERNEL_HEADER)
#include CANDIDATE_KERNEL_HEADER
#endif
#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace ps2x::iop::detail;
namespace ps2x::iop::detail {
struct KernelStateProof {
    static void cpu(std::vector<uint64_t>& out, const IopCpuState& c) {
        out.insert(out.end(),c.gpr.begin(),c.gpr.end());
        out.insert(out.end(),c.cop0.begin(),c.cop0.end());
        for(auto value:{c.hi,c.lo,c.pc,c.pendingLoadReg,c.pendingLoadValue,c.branchTarget})out.push_back(value);
        for(auto value:{c.pendingLoad,c.branchPending,c.stopped,c.yielded,c.exception})out.push_back(value);
    }
    template<class K> static std::vector<uint64_t> snapshot(const K& k) {
        std::vector<uint64_t> out{k.m_nextThreadId,k.m_nextSemaphoreId,k.m_nextEventFlagId,
            static_cast<uint64_t>(k.m_currentThread?k.m_currentThread->id:0),k.m_threads.size()};
        for(const auto& [id,t]:k.m_threads) {
            for(auto value:{static_cast<uint64_t>(id),static_cast<uint64_t>(t.state),static_cast<uint64_t>(t.entry),
              static_cast<uint64_t>(t.stackBase),static_cast<uint64_t>(t.stackSize),static_cast<uint64_t>(t.priority),
              static_cast<uint64_t>(t.initialPriority),static_cast<uint64_t>(t.option),static_cast<uint64_t>(t.attr),
              t.wakeCycle,static_cast<uint64_t>(t.waitId),static_cast<uint64_t>(t.waitBits),
              static_cast<uint64_t>(t.waitMode),static_cast<uint64_t>(t.waitResultAddress),static_cast<uint64_t>(t.wakeupCount)})out.push_back(value);
            cpu(out,t.cpu);
        }
        out.push_back(k.m_semaphores.size());
        for(const auto& [id,s]:k.m_semaphores)
            for(auto value:{static_cast<uint64_t>(id),static_cast<uint64_t>(s.attr),static_cast<uint64_t>(s.option),
              static_cast<uint64_t>(s.current),static_cast<uint64_t>(s.maximum)})out.push_back(value);
        out.push_back(k.m_eventFlags.size());
        for(const auto& [id,e]:k.m_eventFlags)
            for(auto value:{static_cast<uint64_t>(id),static_cast<uint64_t>(e.bits),static_cast<uint64_t>(e.attr),static_cast<uint64_t>(e.option)})out.push_back(value);
        return out;
    }
    template<class K> static bool idle(const K& k) { return k.m_idleSelectionValid; }
};
}

namespace {
uint64_t checks=0,failures=0;
uint64_t selectionFailures=0;
bool expectSelectionControl=false;
struct SelectionControlDetected {};
void check(bool value,const char* what) {++checks;if(!value){++failures;
    if(std::string(what)=="ready selection priority and ID") {
        ++selectionFailures;
        if(expectSelectionControl)throw SelectionControlDetected{};
    }
    std::cerr<<"FAIL "<<what<<'\n';}}
#if defined(CANDIDATE_KERNEL_TYPE)
using CandidateKernel=CANDIDATE_KERNEL_TYPE;
#else
using CandidateKernel=IopKernel;
#endif
uint32_t rng=0x93e1b57u;
uint32_t random32(){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
struct Harness {
    IopMemory aMem,bMem;
    CandidateKernel a{aMem}; IopKernelReference b{bMem};
    uint64_t cycle=0;
    std::vector<uint8_t> aBytes,bBytes;
    void compare(bool allMemory=false) {
        check(KernelStateProof::snapshot(a)==KernelStateProof::snapshot(b),"kernel full public state");
        const size_t length=allMemory?IopMemory::RamSize:0x10000;
        aBytes.resize(length);bBytes.resize(length);
        check(aMem.readRam(0,aBytes.data(),length)&&bMem.readRam(0,bBytes.data(),length)&&aBytes==bBytes,"IOP RAM");
        check(aMem.maxFreeMemory()==bMem.maxFreeMemory(),"allocator free extent");
    }
    void word(uint32_t address,uint32_t value){aMem.write32(address,value);bMem.write32(address,value);}
    uint32_t call(char kind,uint16_t ordinal,uint32_t x=0,uint32_t y=0,uint32_t z=0,uint32_t w=0) {
        IopCpuState ca{},cb{};ca.gpr[4]=cb.gpr[4]=x;ca.gpr[5]=cb.gpr[5]=y;
        ca.gpr[6]=cb.gpr[6]=z;ca.gpr[7]=cb.gpr[7]=w;bool ra=false,rb=false;
        if(kind=='t'){ra=a.dispatchThreadImport(ordinal,ca,cycle);rb=b.dispatchThreadImport(ordinal,cb,cycle);}
        if(kind=='s'){ra=a.dispatchSemaphoreImport(ordinal,ca);rb=b.dispatchSemaphoreImport(ordinal,cb);}
        if(kind=='e'){ra=a.dispatchEventImport(ordinal,ca);rb=b.dispatchEventImport(ordinal,cb);}
        check(ra==rb,"import disposition");std::vector<uint64_t> va,vb;KernelStateProof::cpu(va,ca);KernelStateProof::cpu(vb,cb);
        check(va==vb,"import CPU state");compare();return ca.gpr[2];
    }
    int create(uint32_t priority=64) {
        word(0x1000,0);word(0x1004,random32());word(0x1008,0x10000);
        word(0x100c,0x200);word(0x1010,priority);return static_cast<int>(call('t',4,0x1000));
    }
    std::pair<IopThread*,IopThread*> begin(uint64_t at) {
        cycle=at;auto* ta=a.beginNextReady(at);auto* tb=b.beginNextReady(at);
        check((ta?ta->id:0)==(tb?tb->id:0),"ready selection priority and ID");compare();return {ta,tb};
    }
    void finish(std::pair<IopThread*,IopThread*> pair,unsigned disposition=0,uint64_t when=0) {
        if(!pair.first)return;
        if(disposition==1){a.sleepCurrent(pair.first->cpu);b.sleepCurrent(pair.second->cpu);}
        if(disposition==2){a.delayCurrentUntil(when,pair.first->cpu);b.delayCurrentUntil(when,pair.second->cpu);}
        if(disposition==3){pair.first->cpu.stopped=pair.second->cpu.stopped=true;}
        a.endTimeslice(*pair.first,0x1ffffff0);b.endTimeslice(*pair.second,0x1ffffff0);compare();
    }
    void wake(uint64_t fallback){check(a.nextWakeCycle(fallback)==b.nextWakeCycle(fallback),"next wake cycle");}
};
void focused() {
    Harness h;
    auto ids=std::array<int,3>{h.create(30),h.create(30),h.create(20)};
    for(auto id:ids)h.call('t',6,id);
    h.finish(h.begin(0),2,100);h.finish(h.begin(0),2,100);h.finish(h.begin(0),1);
    for(auto at:{0ull,1ull,99ull,100ull,101ull}){auto p=h.begin(at);h.wake(200);h.finish(p,1);}
    h.begin(102);check(KernelStateProof::idle(h.a),"idle result cached after scan");
    h.call('t',25,ids[2]);h.finish(h.begin(102),1);
    h.call('t',31,ids[0]);h.call('t',29,ids[0]);h.begin(103);h.call('t',31,ids[0]);h.finish(h.begin(104),1);
    int eventA=h.a.createInternalEventFlag(0,0,0),eventB=h.b.createInternalEventFlag(0,0,0);check(eventA==eventB,"internal event IDs");
    h.call('t',25,ids[0]);auto p=h.begin(105);h.call('e',10,eventA,4,0,0x1200);h.finish(p);
    h.begin(106);check(h.a.setInternalEventFlag(eventA,4)==h.b.setInternalEventFlag(eventB,4),"internal event wake");h.finish(h.begin(107),1);
    h.word(0x1100,0);h.word(0x1104,0);h.word(0x1108,0);h.word(0x110c,2);int sem=h.call('s',4,0x1100);
    h.call('t',25,ids[1]);p=h.begin(108);h.call('s',8,sem);h.finish(p);h.begin(109);h.call('s',6,sem);h.finish(h.begin(110),1);
    h.call('t',6,ids[0]);p=h.begin(111);h.call('t',9);h.finish(p);h.a.cleanupDeadThreads();h.b.cleanupDeadThreads();h.compare();
    h.a.terminateThreadsInRange(0x10000,4);h.b.terminateThreadsInRange(0x10000,4);h.compare(true);
    h.a.reset();h.b.reset();h.compare();h.begin(UINT64_MAX-1);h.wake(UINT64_MAX);h.begin(UINT64_MAX);
}
void fuzz() {
    Harness h;std::vector<int> ids;
    for(unsigned i=0;i<24;++i){int id=h.create(1+random32()%126);ids.push_back(id);h.call('t',6,id);h.finish(h.begin(i),1);}
    int ea=h.a.createInternalEventFlag(0,0,0),eb=h.b.createInternalEventFlag(0,0,0);check(ea==eb,"fuzz event IDs");
    h.word(0x1100,0);h.word(0x1104,0);h.word(0x1108,0);h.word(0x110c,3);int sem=h.call('s',4,0x1100);
    for(unsigned i=0;i<4000;++i) {
        h.cycle+=random32()%11;int id=ids[random32()%ids.size()];unsigned action=random32()%13;
        if(action==0)h.call('t',25,id);
        if(action==1)h.call('t',14,id,1+random32()%126);
        if(action==2)h.call('t',29,id);
        if(action==3)h.call('t',31,id);
        if(action==4)h.call('t',18,id);
        if(action==5)h.call('s',6,sem);
        if(action==6){check(h.a.setInternalEventFlag(ea,1)==h.b.setInternalEventFlag(eb,1),"fuzz event change");h.compare();}
        auto pair=h.begin(h.cycle);
        if(pair.first) {
            if(action==7)h.call('e',10,ea,2,0,0x1200);
            if(action==8)h.call('s',8,sem);
            h.finish(pair,action==9?2:1,h.cycle+1+random32()%100);
        }
        h.begin(h.cycle);h.wake(h.cycle+random32()%1000);
    }
    h.compare(true);
}
template<class K> double benchmark(K& kernel) {
    uint64_t sum=0;auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<1000000;++i){sum+=kernel.beginNextReady(i)!=nullptr;sum^=kernel.nextWakeCycle(UINT64_MAX);}
    auto end=std::chrono::steady_clock::now();
    std::cout<<" checksum="<<sum;return std::chrono::duration<double,std::milli>(end-start).count();
}
}
int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--expect-selection-failure") {
        expectSelectionControl=true;
        try {focused();}
        catch(const SelectionControlDetected&) {
            std::cout<<"detected_ready_selection_corruption checks="<<checks<<'\n';return 0;
        }
        std::cerr<<"expected ready-selection corruption was not detected\n";return 1;
    }
    if(argc>1&&std::string(argv[1])=="--benchmark") {
        Harness h;for(unsigned i=0;i<32;++i){int id=h.create();h.call('t',6,id);h.finish(h.begin(0),1);}h.begin(0);
        std::cout<<"reference";double ref=benchmark(h.b);std::cout<<" ms="<<ref<<'\n';
        std::cout<<"candidate";double candidate=benchmark(h.a);std::cout<<" ms="<<candidate<<'\n';return 0;
    }
    focused();fuzz();std::cout<<"exact_checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
