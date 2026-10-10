// SPDX-License-Identifier: GPL-3.0-or-later
// Private authored host-quantum proof; retail bytes are not embedded.
#include "runtime/ps2_vu1_lift.h"
void steppingCases()
{
    using Reason = VU1Interpreter::JobReason;
    Pair p;
    hostJobMode = true;
    constexpr uint32_t quanta[] = {1u,2u,3u,4u,7u,13u,16u,31u,256u,65536u};
    for (uint32_t quantum : quanta) {
        hostJobQuantum = quantum;
        for (uint32_t salt = 0; salt < 64; ++salt) for (uint32_t mask = 0; mask < 16; ++mask) {
            p.reset(); p.seed(salt * 16 + mask);
            p.instruction(0, lowerSpecial(0x38,2,3), upper(0x2a,mask)); // Q / FMAC
            p.instruction(8, lowerSpecial(0x3b,0), upper(0x29,mask,4,2,5)); // WAITQ / ACC sum
            p.instruction(16, lowerSpecial(0x70,2), special(0x29,mask,3,2)); // EFU / ACC
            p.instruction(24, lowerSpecial(0x7b,0), upper(0x28,mask,5,4,6)); // WAITP
            p.instruction(32, (1u << 25) | (mask << 21) | (1u << 16) | (6u << 11), nop);
            p.instruction(40, 0, special(0x1f,0,3,2)); // CLIP
            p.instruction(48, 0, nop | ebit); p.instruction(56,0,nop);
            p.run();
            p.check(p.c->jobStatus() == Reason::Ended, "job-ended");
        }
        // Long-to-short, wrapped, back-to-back XGKICK and bounded draining.
        for (uint32_t offset : {0u,0x3ff0u}) for (uint32_t loops : {0u,1u,31u,255u}) {
            p.reset(); p.seed(loops);
            p.packet(offset, loops, 2, 0, 0x52);
            p.c->state().vi[1] = p.r->state().vi[1] = offset / 16;
            p.instruction(0, lowerSpecial(0x6c,1), nop | ebit); p.instruction(8,0,nop);
            p.run(); p.check(p.c->jobStatus() == Reason::Ended, "drained-kick-end");
        }
        // Every small issue ceiling remains cumulative across host steps.
        for (uint32_t budget = 0; budget < 32; ++budget) {
            p.reset(); p.seed(budget);
            p.instruction(0, 0, upper(0x2a)); p.instruction(8, (0x20u << 25) | 0x7feu, nop);
            p.instruction(16,0,nop);
            p.run(false,budget);
            p.check(p.c->jobStatus() == Reason::BudgetExhausted, "issue-ceiling");
            p.run(true,13,0,0x18,0x42);
        }
        for (uint32_t halt : {0x10000000u,0x08000000u}) for (bool branch : {false,true}) {
            p.reset(); p.seed(5);
            p.c->state().dBitEnabled = p.r->state().dBitEnabled = true;
            p.c->state().tBitEnabled = p.r->state().tBitEnabled = true;
            p.instruction(0,branch ? (0x20u << 25) | 1u : 0u, upper(0x2a) | halt);
            p.instruction(8,0,nop); p.instruction(16,0,nop | ebit); p.instruction(24,0,nop);
            p.run();
            p.check(p.c->jobStatus() == (halt == 0x10000000u ? Reason::StoppedD : Reason::StoppedT), "guest-stop-kind");
            p.run(true,64,0,0x63,0x64);
        }
    }
    // Capture a pair across a hazard stall, then mutate code between quanta.
    for (uint32_t quantum : {1u,2u,3u}) {
        p.reset(); p.seed(111);
        p.instruction(0,0,upper(0x2a,15,3,2,4));
        p.instruction(8,0,upper(0x28,15,4,2,5));
        p.instruction(16,0,nop | ebit); p.instruction(24,0,nop);
        ++calls;
        p.r->execute(p.rm->getVU1Code(),PS2_VU1_CODE_SIZE,p.rm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.rm.get());
        p.check(p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get()), "capture-begin");
        p.c->stepJob(2);
        const uint64_t replacement = uint64_t(upper(0x2c,15,4,2,5)) << 32;
        p.cm->write64(PS2_VU1_CODE_BASE+8,replacement);
        while (p.c->jobActive()) p.c->stepJob(quantum);
        p.compare();
    }
    // PATH1 callback mutates the captured stalled pair in each own code image.
    p.reset(); p.seed(23); p.packet(0,1,2,0,0x37);
    p.instruction(0,lowerSpecial(0x6c,0),upper(0x2a,15,3,2,4));
    p.instruction(8,0,upper(0x28,15,4,2,5));
    p.instruction(16,0,nop|ebit); p.instruction(24,0,nop);
    auto mutation = [&](bool candidate,const uint8_t *data,uint32_t bytes) {
        (candidate ? p.cp : p.rp).emplace_back(data,data+bytes);
        auto &mem = candidate ? p.cm : p.rm;
        mem->write64(PS2_VU1_CODE_BASE+8,uint64_t(upper(0x2c,15,4,2,5))<<32);
    };
    p.cm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){mutation(true,d,n);});
    p.rm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){mutation(false,d,n);});
    hostJobQuantum = 1; p.run();
    p.cm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){p.cp.emplace_back(d,d+n);});
    p.rm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){p.rp.emplace_back(d,d+n);});
    // Caller environment includes rounding and existing exception flags.
    for (int rounding : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        p.reset(); p.seed(77);
        p.instruction(0,0,upper(0x29)|ebit); p.instruction(8,0,nop);
        std::fesetround(rounding); std::feclearexcept(FE_ALL_EXCEPT); std::feraiseexcept(FE_DIVBYZERO);
        const int exceptions = std::fetestexcept(FE_ALL_EXCEPT);
        p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
        while (p.c->jobActive()) {
            p.c->stepJob(1);
            p.check(std::fegetround() == rounding && std::fetestexcept(FE_ALL_EXCEPT) == exceptions,"fp-environment");
        }
    }
    std::fesetround(FE_TONEAREST); std::feclearexcept(FE_ALL_EXCEPT);
    // Exceptions from a real PATH1 callback leave the host environment intact.
    p.reset(); p.packet(0,1,2,0,0x73); p.instruction(0,lowerSpecial(0x6c,0),nop|ebit); p.instruction(8,0,nop);
    p.cm->setGifPacketCallback([](const uint8_t *,uint32_t){throw std::runtime_error("authored callback");});
    std::fesetround(FE_DOWNWARD); std::feraiseexcept(FE_DIVBYZERO);
    const int previous = std::fetestexcept(FE_ALL_EXCEPT);
    p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
    bool caught = false;
    try { while(p.c->jobActive()) p.c->stepJob(1); } catch(const std::runtime_error &) { caught = true; }
    p.check(caught && !p.c->jobActive() && p.c->jobStatus() == Reason::Faulted,"job-exception");
    p.check(std::fegetround() == FE_DOWNWARD && std::fetestexcept(FE_ALL_EXCEPT) == previous,"exception-fp-environment");
    std::fesetround(FE_TONEAREST); std::feclearexcept(FE_ALL_EXCEPT);
    // Reset/cancel discards unsent packet work; repeated stepping stays inert.
    p.reset(); p.packet(0,255,2,0,0x39); p.instruction(0,lowerSpecial(0x6c,0),nop|ebit); p.instruction(8,0,nop);
    p.cm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){p.cp.emplace_back(d,d+n);});
    p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
    p.c->stepJob(2); p.c->cancelJob();
    p.check(!p.c->jobActive() && p.c->jobStatus() == Reason::Cancelled,"job-cancel");
    p.c->stepJob(65536); p.check(p.cp.empty(),"cancel-unsent-kick");
    p.c->reset(); p.check(!p.c->jobActive(),"reset-job");
    // Reject overlapping launch without relatching the active command's state.
    p.reset(); p.instruction(0,0,nop|ebit); p.instruction(8,0,nop);
    p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get(),0,17,31);
    const auto activeState = p.c->state();
    p.check(!p.c->beginResume(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get(),99,100),"job-overlap-rejected");
    p.check(std::memcmp(&activeState,&p.c->state(),sizeof(activeState)) == 0,"job-overlap-no-relatch");
    for (bool resume : {false,true}) {
        bool rejected = false;
        try {
            if (resume) p.c->resume(p.rm->getVU1Code(),PS2_VU1_CODE_SIZE,p.rm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.rm.get(),99,100);
            else p.c->execute(p.rm->getVU1Code(),PS2_VU1_CODE_SIZE,p.rm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.rm.get(),8,99,100);
        } catch (const std::logic_error &) { rejected = true; }
        p.check(rejected && p.c->jobActive(),"job-legacy-overlap-rejected");
        p.check(std::memcmp(&activeState,&p.c->state(),sizeof(activeState)) == 0,"job-legacy-overlap-no-relatch");
    }
    p.check(p.c->stepJob(0).cyclesAdvanced == 0 && p.c->jobActive(),"zero-quantum");
    p.c->cancelJob();
    // The same exclusion applies inside real PATH1 callbacks. A callback may
    // catch the misuse; the original bounded job then completes unchanged.
    for (bool resume : {false,true}) {
        p.reset(); p.packet(0,1,2,0,0x68);
        p.instruction(0,lowerSpecial(0x6c,0),nop|ebit); p.instruction(8,0,nop);
        bool rejected = false;
        p.cm->setGifPacketCallback([&](const uint8_t *d,uint32_t n) {
            p.cp.emplace_back(d,d+n);
            try {
                if (resume) p.c->resume(p.rm->getVU1Code(),PS2_VU1_CODE_SIZE,p.rm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.rm.get());
                else p.c->execute(p.rm->getVU1Code(),PS2_VU1_CODE_SIZE,p.rm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.rm.get());
            } catch (const std::logic_error &) { rejected = true; }
        });
        hostJobQuantum = 1; p.run();
        p.check(rejected && p.c->jobStatus()==Reason::Ended,"job-callback-legacy-overlap");
    }
    p.cm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){p.cp.emplace_back(d,d+n);});
    // A packet callback reset cancels an already captured second XGKICK.
    p.reset(); p.packet(0,1,2,0,0x53); p.packet(128,1,2,0,0x64);
    p.c->state().vi[1] = 8;
    p.instruction(0,lowerSpecial(0x6c,0),nop);
    p.instruction(8,lowerSpecial(0x6c,1),nop|ebit); p.instruction(16,0,nop);
    p.cm->setGifPacketCallback([&](const uint8_t *d,uint32_t n){p.cp.emplace_back(d,d+n);p.c->reset();});
    p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
    while(p.c->jobActive()) p.c->stepJob(1);
    p.check(p.cp.size() == 1 && p.c->state().cycles == 0,"callback-reset-cancels-captured-kick");
    p.c->stepJob(65536); p.check(p.cp.size() == 1,"callback-reset-no-old-packets");
    // Reserved and invalid targets are distinct from budget/normal completion.
    p.reset(); p.instruction(0,0xffffffffu,0xffffffffu);
    p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
    p.check(p.c->stepJob(1).reason == Reason::ReservedInstruction,"job-reserved");
    p.c->reset(); p.c->beginExecute(p.cm->getVU1Code(),4,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
    p.check(p.c->stepJob(1).reason == Reason::InvalidPc,"job-invalid-pc");
    // A malformed tag can fault during an already-decoded XGKICK's stall.
    // A host cutoff must still issue that captured pair, just as the original
    // loop does before testing its stop flag at the next iteration. A fault
    // during E-bit draining must retain the exact drained completion state.
    for (uint32_t quantum : {1u,2u,3u,4u,7u}) for (bool ended : {false,true}) {
        p.reset();
        const uint64_t oversizedTag = 0xffffu; // packed NUM32767/NREG16 > buffer
        std::memcpy(p.cm->getVU1Data(),&oversizedTag,sizeof(oversizedTag));
        std::memcpy(p.rm->getVU1Data(),&oversizedTag,sizeof(oversizedTag));
        p.instruction(0,lowerSpecial(0x6c,0),nop|(ended ? ebit : 0u));
        p.instruction(8,ended ? 0u : lowerSpecial(0x6c,0),nop);
        p.instruction(16,0,nop|ebit); p.instruction(24,0,nop);
        hostJobQuantum=quantum; p.run(false,64);
        p.check(p.c->jobStatus()==Reason::ReservedInstruction,"job-packet-fault-kind");
    }
    // Unit0 uses its own tracked code/data and address mask, with the same
    // bounded host continuation contract and reference arithmetic.
    p.c = std::make_unique<VU1Interpreter>(VU1Interpreter::Unit::VU0);
    p.r = std::make_unique<VU1InterpreterRef>(VU1InterpreterRef::Unit::VU0);
    for (uint32_t quantum : {1u,7u,256u}) for (uint32_t mask=0;mask<16;++mask) {
        p.reset(); p.seed(mask);
        const uint64_t first = uint64_t(upper(0x2a,mask,3,2,4))<<32;
        const uint64_t second = uint64_t(upper(0x29,mask,4,2,5)|ebit)<<32;
        const uint64_t third = uint64_t(nop)<<32;
        for (auto *memory : {p.cm.get(),p.rm.get()}) {
            memory->write64(PS2_VU0_CODE_BASE,first);
            memory->write64(PS2_VU0_CODE_BASE+8,second);
            memory->write64(PS2_VU0_CODE_BASE+16,third);
        }
        ++calls;
        p.r->execute(p.rm->getVU0Code(),PS2_VU0_CODE_SIZE,p.rm->getVU0Data(),PS2_VU0_DATA_SIZE,p.gs,p.rm.get());
        p.c->beginExecute(p.cm->getVU0Code(),PS2_VU0_CODE_SIZE,p.cm->getVU0Data(),PS2_VU0_DATA_SIZE,p.gs,p.cm.get());
        while(p.c->jobActive()) p.check(p.c->stepJob(quantum).cyclesAdvanced<=quantum,"vu0-quantum");
        p.compare();
        p.check(std::memcmp(p.cm->getVU0Data(),p.rm->getVU0Data(),PS2_VU0_DATA_SIZE)==0,"vu0-data");
    }
    hostJobMode = false;
}

void jobLiftBypassCase()
{
    Pair p;
    p.reset(); p.instruction(0,0,nop|ebit); p.instruction(8,0,nop);
    static uint32_t called = 0;
    const vl::Registration registration{vl::hashCode(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE),0,
        [](vl::Run &run){++called;run.endCycle=run.cycle+100000u;}};
    const vl::Registrar registry(&registration,1);
    p.c->beginExecute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get());
    while(p.c->jobActive()) {
        const auto step=p.c->stepJob(1);
        p.check(step.cyclesAdvanced<=1 && called==0,"job-bypasses-unbounded-lift");
    }
    // Ordinary execute remains compatible with the opt-in upstream lift.
    p.c->execute(p.cm->getVU1Code(),PS2_VU1_CODE_SIZE,p.cm->getVU1Data(),PS2_VU1_DATA_SIZE,p.gs,p.cm.get(),0,0,0,1);
    p.check(called==1,"legacy-lift-still-enabled");
}
