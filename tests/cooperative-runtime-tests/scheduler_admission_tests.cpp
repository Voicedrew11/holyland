// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "ps2_syscalls.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
namespace ps2_stubs { void sceGsSyncPath(uint8_t *,R5900Context *,PS2Runtime *); }

namespace {
unsigned checks=0, timers=0, fields=0, packets=0, completions=0;
bool inGuest=false, cancelMode=false, faultMode=false, yielded=false,idleMode=false,pollMode=false,lifecycleMode=false,aheadMode=false;
bool secondaryWaiting=false,secondaryDeleted=false,pollDone=false;
unsigned deadResumes=0,replacementRuns=0,pollEntries=0;
uint64_t firstPacketField=0;
int secondaryId=0;
std::chrono::steady_clock::time_point sentAt{},firstPacketAt{};
constexpr uint32_t setupPc=0x180000,sendPc=setupPc+4,waitPc=setupPc+8,finishPc=setupPc+12;
constexpr uint32_t timerPc=setupPc+16,fieldPc=setupPc+20,otherPc=setupPc+24;
constexpr uint32_t timerPollPc=setupPc+28,deadPc=setupPc+32,replacementPc=setupPc+36;
void check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
void pair(PS2Memory&m,uint32_t at,uint32_t lo,uint32_t hi=0x2ff){m.write64(PS2_VU1_CODE_BASE+at,lo|(uint64_t(hi)<<32));}
void timer(uint8_t*,R5900Context*c,PS2Runtime*r){check(!inGuest,"timer never reenters native issuer/callback");++timers;
 auto& m=r->memory();const auto mode=m.readIORegister(0x10000010);m.writeIORegister(0x10000010,mode|0x400);
 if(cancelMode&&timers==1)m.writeIORegister(0x10003C10,1);
 if(lifecycleMode&&secondaryWaiting&&!secondaryDeleted){
   auto&ee=r->eeScheduler();uint32_t owned=0;
   check(ee.terminateThread(secondaryId,owned,true)==0,"vanished waiter terminates");
   check(ee.deleteThread(secondaryId,owned)==0,"vanished waiter deletes");secondaryDeleted=true;
   EeThreadCreateParams p{};p.entry=replacementPc;p.priority=0;p.stack=0x8000;p.stackSize=0x1000;
   for(int id=3;id<=255;++id)check(ee.createThread(p)==id,"authored dormant records force real ID wrap");
   const int replacement=ee.createThread(p);check(replacement==secondaryId,"replacement caller reuses deleted ID");
   check(ee.startThread(replacement,0,*c,true)==0,"replacement ready");
 }
 if(pollMode&&timers==1&&m.vif1DmaBusy()){c->pc=timerPollPc;return;}
 c->pc=0;}
void timerPoll(uint8_t*,R5900Context*c,PS2Runtime*r){++pollEntries;
 if((r->memory().readIORegister(0x10009000)&0x100u)!=0u){
   check(r->eeScheduler().checkpointDue(32),"polling ISR reaches cooperative safe point");
   c->pc=timerPollPc;throw EeDispatcherTransfer{};
 }
 pollDone=true;c->pc=0;}
unsigned presentationDeferrals=0;
void field(uint8_t*,R5900Context*c,PS2Runtime*r){check(!inGuest,"field never reenters native issuer/callback");++fields;
 if(r->vu1().jobActive()){
   check(!r->gs().latchHostPresentationFrame(),"real field callback sees producer admission deferred");++presentationDeferrals;
 }
 c->pc=0;}
void other(uint8_t*ram,R5900Context*c,PS2Runtime*r){if(!cancelMode&&!faultMode)check(r->memory().vif1DmaBusy(),"unrelated EE code executes while DMA busy");yielded=true;
 if(lifecycleMode){secondaryWaiting=true;c->r[31]=_mm_cvtsi32_si128(deadPc);r->waitCooperativeVif1(c,ram,false);check(false,"secondary wait suspends");}
 c->pc=0;}
void dead(uint8_t*,R5900Context*c,PS2Runtime*){++deadResumes;c->pc=0;}
void replacement(uint8_t*,R5900Context*c,PS2Runtime*){++replacementRuns;c->pc=0;}
void finish(uint8_t*,R5900Context*c,PS2Runtime*r){
 ++completions;check(!r->vu1().jobActive(),"terminal barrier observes no active VU");
 check(static_cast<int32_t>(getRegU32(c,2))==((cancelMode||faultMode)?-1:0),"native completion sets V0 success/cancel exactly");
 check(!r->memory().vif1DmaBusy(),"terminal barrier retires/cancels DMA");
 if(!cancelMode&&!faultMode){check(packets==40,"ordered XGKICK packets all submitted before SyncPath returns");
   if(!aheadMode&&!pollMode){check(timers>=2,"real timer ISR serviced between bounded quanta");check(fields>=1,"real fields serviced while VU busy");}
   if(!idleMode&&!aheadMode&&!pollMode)check(yielded,"ready unrelated guest made progress");
   if(pollMode){check(timers>=1,"polling ISR entered normally");
     check(pollDone&&pollEntries>1,"already-entered polling ISR permits bounded VU progress without nesting IRQs");}
   if(lifecycleMode)check(secondaryDeleted&&deadResumes==0&&replacementRuns==1,"stale external token never wakes replacement caller");
   if(aheadMode){check(firstPacketAt-sentAt<std::chrono::milliseconds(500),"ahead-of-wall idle service remains bounded");
     check(firstPacketField==0,"ahead-of-wall idle job starts before waiting for first future field");}
 }
 check(r->gs().latchHostPresentationFrame(),"native barrier completion admits a host frame");
 if(!cancelMode&&!faultMode&&!aheadMode&&!pollMode)check(presentationDeferrals>0,"real physical fields ran while host snapshots were deferred");
 c->pc=0;r->requestStop();}
void wait(uint8_t*ram,R5900Context*c,PS2Runtime*r){
 inGuest=true;check(r->memory().vif1DmaBusy(),"blocking SyncPath sees true busy");
 c->r[4]=_mm_setzero_si128();c->r[31]=_mm_cvtsi32_si128(finishPc);c->pc=finishPc;
 inGuest=false;ps2_stubs::sceGsSyncPath(ram,c,r);
 check(false,"busy SyncPath must suspend rather than spin/complete immediately");}
void send(uint8_t*,R5900Context*c,PS2Runtime*r){
 inGuest=true;auto&m=r->memory();const uint32_t launch[4]={0x14000000,0,0,0};std::memcpy(m.getRDRAM()+0x1000,launch,16);
 m.writeIORegister(0x10009010,0x1000);m.writeIORegister(0x10009020,1);m.writeIORegister(0x10009000,0x181);
 check(packets==0&&!r->vu1().jobActive(),"DMA start accepts owned input without running native VU stack");
 std::memset(m.getRDRAM()+0x1000,0,16); // original source lifetime ends here
 sentAt=std::chrono::steady_clock::now();
 if(aheadMode)r->eeScheduler().accountCycles(400000000u);
 c->pc=waitPc;inGuest=false;
}
void setup(uint8_t*,R5900Context*c,PS2Runtime*r){
 auto&m=r->memory();m.writeIORegister(0x10000020,60000);m.writeIORegister(0x10000010,0x1C1);m.writeIORegister(0x10000000,0);
 if(!idleMode&&!aheadMode){EeThreadCreateParams p{};p.entry=otherPc;p.priority=0;p.stack=0x8000;p.stackSize=0x1000;
 const int id=r->eeScheduler().createThread(p);secondaryId=id;check(id>1,"secondary guest created");check(r->eeScheduler().startThread(id,0,*c,false)==0,"secondary guest ready");}
 c->pc=sendPc;
}
}
int main(int argc,char**argv){try{
 if(argc>1){cancelMode=std::strcmp(argv[1],"cancel")==0;faultMode=std::strcmp(argv[1],"fault")==0;idleMode=std::strcmp(argv[1],"idle")==0;
 pollMode=std::strcmp(argv[1],"poll")==0;lifecycleMode=std::strcmp(argv[1],"lifecycle")==0;aheadMode=std::strcmp(argv[1],"ahead")==0;}
 auto r=std::make_unique<PS2Runtime>();check(r->memory().initialize(),"real RAM initializes");check(r->syncCoreSubsystems(),"real VU/GS bindings initialize");
 if(argc>1&&std::strcmp(argv[1],"reentry")==0){
   auto&m=r->memory();m.setGifArbiter(nullptr);const uint64_t tag=0x0800000000008000ull;std::memcpy(m.getVU1Data(),&tag,8);
   pair(m,0,0x8000003Cu|((0x6Cu&0x7Cu)<<4)|(1u<<11),0x400002FFu);pair(m,8,0);
   m.setGifPacketCallback([&](const uint8_t*,uint32_t){++packets;
     if(packets==1){m.cancelVif1Work();const uint32_t fresh[4]={0x14000000,0,0,0};
       m.processVIF1Data(reinterpret_cast<const uint8_t*>(fresh),16);r->serviceCooperativeVif1();
       check(!r->vu1().jobActive()&&m.vif1WorkPending(),"recursive service defers fresh epoch launch");}
   });
   const uint32_t launch[4]={0x14000000,0,0,0};std::memcpy(m.getRDRAM()+0x1000,launch,16);
   m.writeIORegister(0x10009010,0x1000);m.writeIORegister(0x10009020,1);m.writeIORegister(0x10009000,0x181);
   for(unsigned n=0;n<100&&(m.vif1WorkPending()||r->vu1().jobActive());++n)r->serviceCooperativeVif1();
   check(packets==2&&!m.vif1WorkPending(),"callback-reset fresh stream launches exactly once after outer step");
   check(!m.vif1DmaBusy()&&(m.readIORegister(0x1000E010)&2)==0,"old cancelled DMA never reports completion");
   std::cout<<"cooperative-reentry checks="<<checks<<" packets="<<packets<<"\n";return 0;
 }
 auto&m=r->memory();m.setGifArbiter(nullptr);m.setGifPacketCallback([&](const uint8_t* data,uint32_t bytes){
  check(!inGuest,"PATH1 submitted by outer executor service");check(bytes==16&&data[1]==0x80,"owned complete packet order");
  inGuest=true;if(packets==0){firstPacketAt=std::chrono::steady_clock::now();firstPacketField=m.gs().vsyncTick.load();}
  ++packets;if(!aheadMode)std::this_thread::sleep_for(std::chrono::milliseconds(1));inGuest=false;});
 r->vu1().state().vi[2]=40;
 const uint64_t packetTag=0x0800000000008000ull;std::memcpy(m.getVU1Data(),&packetTag,8);
 if(faultMode){pair(m,0,0xFE000000u);}
 else{
  for(unsigned p=0;p<34;++p)pair(m,p*8,0);
  pair(m,0,0x8000003Cu|((0x6Cu&0x7Cu)<<4)|(1u<<11));
  pair(m,8,(0x08u<<25)|(3u<<16)|(3u<<11)|1u);
  pair(m,240,(0x29u<<25)|(2u<<16)|(3u<<11)|0x7E1u);
  pair(m,256,0,0x400002FFu);pair(m,264,0);
 }
 r->registerFunction(setupPc,setup);r->registerFunction(sendPc,send);r->registerFunction(waitPc,wait);r->registerFunction(finishPc,finish);
 r->registerFunction(timerPc,timer);r->registerFunction(fieldPc,field);r->registerFunction(otherPc,other);
 r->registerFunction(timerPollPc,timerPoll);r->registerFunction(deadPc,dead);r->registerFunction(replacementPc,replacement);
 R5900Context context{};context.pc=setupPc;auto&ee=r->eeScheduler();ee.reset(m.getRDRAM(),context);
 check(ee.addIrqHandler(false,9,timerPc,true,0,0,0)>0,"timer handler registers");ee.setIrqCauseEnabled(false,9,true);
 check(ee.addIrqHandler(false,2,fieldPc,true,0,0,0)>0,"field handler registers");ee.setIrqCauseEnabled(false,2,true);
 ee.run();check(completions==1,"wait continuation executes once");
 std::cout<<"cooperative-scheduler checks="<<checks<<" packets="<<packets<<" timers="<<timers<<" fields="<<fields<<"\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<" checks="<<checks<<" packets="<<packets<<" timers="<<timers<<"\n";return 1;}}
