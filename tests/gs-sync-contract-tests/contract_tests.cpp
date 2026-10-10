// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "runtime/ee_scheduler.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {
constexpr uint32_t kWait=0x00160000u,kResume=0x00160010u;
constexpr uint32_t kFlag=0x1800u,kSentinel=0x1808u,kCsr=0x1810u;
constexpr uint64_t kSentinelBits=0x9e3779b97f4a7c15ull;
constexpr uint64_t kSeedCsr=0x1234000000000068ull;
constexpr uint64_t kDetachedCsr=0xdeadbeef01234567ull;
constexpr uint32_t kDetachedFlag=0xcafebabeu;
size_t calls{},checks{},failures{};
bool progressive{},fixed{};
std::string expectedCategory;
size_t unrelatedFailures{};
void check(bool yes,const char *why){
    ++checks;
    if(!yes){
        ++failures;
        const bool expected = expectedCategory=="csr"
            ? std::strcmp(why,"registered second output contains full sampled CSR")==0 || std::strcmp(why,"alternate SDK wait derives current field from second output")==0
            : expectedCategory=="field" && std::strcmp(why,"actual SyncV agrees with original SDK current CSR field contract")==0;
        if(!expected)++unrelatedFailures;
        std::fprintf(stderr,"FAIL: %s\n",why);
    }
}
template<class T> T load(const uint8_t *rdram,uint32_t address){T v{};std::memcpy(&v,rdram+address,sizeof(v));return v;}
template<class T> void store(uint8_t *rdram,uint32_t address,T value){std::memcpy(rdram+address,&value,sizeof(value));}
void wait(uint8_t *rdram,R5900Context *ctx,PS2Runtime *rt){
    const bool registerOutputs=calls!=1u&&calls!=4u;
    store(rdram,kSentinel,kSentinelBits);
    if(registerOutputs){
        store(rdram,kFlag,kDetachedFlag);store(rdram,kCsr,kDetachedCsr);
        SET_GPR_U32(ctx,4,kFlag);SET_GPR_U32(ctx,5,kCsr);
        ps2_syscalls::SetVSyncFlag(rdram,ctx,rt);
        check(getRegU32(ctx,2)==0u,"actual SetVSyncFlag succeeds");
        check(load<uint32_t>(rdram,kFlag)==0u,"SetVSyncFlag clears first output");
        check(load<uint64_t>(rdram,kCsr)==0u,"SetVSyncFlag clears second output");
    }else{
        store(rdram,kFlag,kDetachedFlag);store(rdram,kCsr,kDetachedCsr);
    }
    ctx->pc=kResume;
    if(fixed) ps2_syscalls::WaitVSyncTick(rdram,ctx,rt,7);
    else ps2_stubs::sceGsSyncV(rdram,ctx,rt);
}
void resume(uint8_t *rdram,R5900Context *ctx,PS2Runtime *rt){
    const uint64_t csr=rt->memory().gs().csr.load(std::memory_order_acquire);
    const uint64_t tick=rt->eeScheduler().currentVSyncTick();
    const uint32_t result=getRegU32(ctx,2);
    const bool registered=calls!=1u&&calls!=4u;
    check(tick==calls+1u,"six actual waits receive sequential field starts");
    check((csr&~0x2000ull)==(kSeedCsr&~0x2000ull),"field publication preserves every other CSR bit");
    check(result==(fixed?7u:progressive?1u:static_cast<uint32_t>((csr>>13u)&1u)),"actual SyncV agrees with original SDK current CSR field contract");
    if(registered){
        check(load<uint32_t>(rdram,kFlag)==1u,"registered completion flag is set");
        check(load<uint64_t>(rdram,kCsr)==csr,"registered second output contains full sampled CSR");
        check(((load<uint64_t>(rdram,kCsr)>>13u)&1u)==((csr>>13u)&1u),"alternate SDK wait derives current field from second output");
    }else{
        check(load<uint32_t>(rdram,kFlag)==kDetachedFlag,"flag registration is one-shot");
        check(load<uint64_t>(rdram,kCsr)==kDetachedCsr,"CSR registration is one-shot");
    }
    check(load<uint64_t>(rdram,kSentinel)==kSentinelBits,"output writes preserve adjacent sentinel");
    std::printf("field=%llu result=%u csr=%016llx registered=%d\n",static_cast<unsigned long long>(tick),result,static_cast<unsigned long long>(csr),registered);
    if(++calls==6u){ctx->pc=0u;rt->requestStop();}else ctx->pc=kWait;
}
}
int main(int argc,char **argv){
    const std::string mode=argc>1?argv[1]:"interlace";
    expectedCategory=argc>2?argv[2]:"";
    if(argc>3||(mode!="interlace"&&mode!="progressive"&&mode!="fixed") ||
       (!expectedCategory.empty()&&expectedCategory!="csr"&&expectedCategory!="field"))return EXIT_FAILURE;
    progressive=mode=="progressive";fixed=mode=="fixed";
    auto rt=std::make_unique<PS2Runtime>();
    check(rt->memory().initialize(),"native runtime memory initializes");
    uint8_t *rdram=rt->memory().getRDRAM();
    R5900Context config{};
    SET_GPR_U32(&config,4,0u);SET_GPR_U32(&config,5,progressive?0u:1u);
    SET_GPR_U32(&config,6,2u);SET_GPR_U32(&config,7,1u);
    ps2_stubs::sceGsResetGraph(rdram,&config,rt.get());
    check(getRegU32(&config,2)==0u,"actual ResetGraph selects requested display interlace mode");
    rt->memory().gs().csr.store(kSeedCsr,std::memory_order_release);
    rt->registerFunction(kWait,wait);rt->registerFunction(kResume,resume);
    R5900Context c{};c.pc=kWait;
    rt->eeScheduler().reset(rdram,c);
    rt->eeScheduler().run();
    check(calls==6u,"all authored SDK waits complete");
    std::printf("%zu checks, %zu failures\n",checks,failures);
    if(!expectedCategory.empty()){
        // Four full-CSR mismatches and two registered odd FIELD masks fail.
        const size_t expectedFailures=6u;
        if(failures==expectedFailures&&unrelatedFailures==0u){
            std::printf("EXPECTED CATEGORY REJECTION: %s, %zu checks rejected\n",expectedCategory.c_str(),failures);
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    }
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
