// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
namespace {
constexpr uint32_t kMainWait=0x160000u,kMainResume=0x160010u;
constexpr uint32_t kPriorityWait=0x160020u,kPriorityResume=0x160030u,kPriorityFinish=0x160040u;
constexpr uint32_t kFlag=0x1800u,kCsr=0x1810u;
constexpr uint64_t kSeedCsr=0x1234000000000068ull;
uint64_t firstCsr{};
size_t checks{},failures{},unrelatedFailures{};
bool expectWakeControl{};
void check(bool c,const char *m){
    ++checks;
    if(!c){
        ++failures;
        const bool expected=expectWakeControl &&
            (std::strcmp(m,"ordinary SDK returns current CSR at actual continuation resume")==0 ||
             std::strcmp(m,"ordinary return and captured kernel output deliberately differ after delayed resume")==0);
        if(!expected)++unrelatedFailures;
        std::fprintf(stderr,"FAIL: %s\n",m);
    }
}
void mainWait(uint8_t *rdram,R5900Context *c,PS2Runtime *r){
    SET_GPR_U32(c,4,kFlag);SET_GPR_U32(c,5,kCsr);
    ps2_syscalls::SetVSyncFlag(rdram,c,r);
    c->pc=kMainResume;
    ps2_stubs::sceGsSyncV(rdram,c,r);
}
void mainResume(uint8_t *rdram,R5900Context *c,PS2Runtime *r){
    const uint64_t now=r->memory().gs().csr.load(std::memory_order_acquire);
    uint64_t captured{};uint32_t flag{};
    std::memcpy(&captured,rdram+kCsr,sizeof(captured));std::memcpy(&flag,rdram+kFlag,sizeof(flag));
    check(r->eeScheduler().currentVSyncTick()==2u,"suspended waiter resumes only after the opposite physical field");
    check(firstCsr!=now,"authored priority thread genuinely delayed across a FIELD transition");
    check(captured==firstCsr,"kernel second output retains the CSR captured at wake");
    check(flag==1u,"kernel first output remains completed during suspension");
    check(getRegU32(c,2)==static_cast<uint32_t>((now>>13u)&1u),"ordinary SDK returns current CSR at actual continuation resume");
    check(getRegU32(c,2)!=static_cast<uint32_t>((captured>>13u)&1u),"ordinary return and captured kernel output deliberately differ after delayed resume");
    std::printf("wake CSR=%016llx resume CSR=%016llx result=%u\n",static_cast<unsigned long long>(captured),static_cast<unsigned long long>(now),getRegU32(c,2));
    c->pc=0u;r->requestStop();
}
void priorityWait(uint8_t *rd,R5900Context *c,PS2Runtime *r){c->pc=kPriorityResume;ps2_syscalls::WaitVSyncTick(rd,c,r,99);}
void priorityResume(uint8_t *rd,R5900Context *c,PS2Runtime *r){
    check(r->eeScheduler().currentVSyncTick()==1u,"higher-priority guest wakes on first field");
    firstCsr=r->memory().gs().csr.load(std::memory_order_acquire);
    check(r->eeScheduler().suspendThread(1,false)==0,"higher-priority guest suspends Ready main before its SDK continuation");
    c->pc=kPriorityFinish;ps2_syscalls::WaitVSyncTick(rd,c,r,99);
}
void priorityFinish(uint8_t *,R5900Context *c,PS2Runtime *r){
    check(r->eeScheduler().currentVSyncTick()==2u,"priority guest waits for the opposite field");
    check(r->eeScheduler().resumeThread(1,false)==0,"priority guest releases the suspended main continuation");
    c->pc=0u;
}
}
int main(int argc,char **argv){
    expectWakeControl=argc==2&&std::strcmp(argv[1],"--expect-wake-only")==0;
    if(argc>2||(argc==2&&!expectWakeControl))return EXIT_FAILURE;
    auto r=std::make_unique<PS2Runtime>();check(r->memory().initialize(),"memory initializes");
    auto *rd=r->memory().getRDRAM();R5900Context cfg{};
    SET_GPR_U32(&cfg,4,0);SET_GPR_U32(&cfg,5,1);SET_GPR_U32(&cfg,6,2);SET_GPR_U32(&cfg,7,1);
    ps2_stubs::sceGsResetGraph(rd,&cfg,r.get());check(getRegU32(&cfg,2)==0u,"actual ResetGraph configures interlace");
    r->memory().gs().csr.store(kSeedCsr,std::memory_order_release);
    r->registerFunction(kMainWait,mainWait);r->registerFunction(kMainResume,mainResume);
    r->registerFunction(kPriorityWait,priorityWait);r->registerFunction(kPriorityResume,priorityResume);r->registerFunction(kPriorityFinish,priorityFinish);
    R5900Context main{};main.pc=kMainWait;
    auto &ee=r->eeScheduler();ee.reset(rd,main);
    int oldPriority{};
    check(ee.changePriority(1,64,false,oldPriority)==0,"main has lower authored priority");
    EeThreadCreateParams param{};param.entry=kPriorityWait;param.stack=0x30000;param.stackSize=0x4000;param.priority=0;
    int id=ee.createThread(param);check(id>1,"priority thread creates");
    check(ee.startThread(id,0,main,false)==0,"priority thread starts");
    ee.run();std::printf("%zu checks, %zu failures\n",checks,failures);
    if(expectWakeControl){
        if(failures==2u&&unrelatedFailures==0u){
            std::printf("EXPECTED CATEGORY REJECTION: delayed wake sampling\n");
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    }
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
