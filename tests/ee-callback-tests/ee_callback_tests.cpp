#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifndef EXPECT_STALE_CALLBACKS
#define EXPECT_STALE_CALLBACKS 0
#endif

namespace {
constexpr uint32_t kBoot = 0x160000u;
constexpr uint32_t kPreempt = kBoot + 0x10u;
constexpr uint32_t kSequence = kBoot + 0x20u;
constexpr uint32_t kFinish = kBoot + 0x30u;
constexpr uint32_t kCallback = kBoot + 0x100u;
constexpr uint32_t kContinuation = kBoot + 0x110u;
constexpr uint32_t kInvalidateBeforeEntry = kBoot + 0x200u;
constexpr uint32_t kInvalidateStarted = kBoot + 0x210u;
constexpr uint32_t kCallerSp = 0x80000u;
constexpr uint32_t kCallerRa = 0x12345000u;
constexpr uint32_t kCallerCookie = 0x76543210u;
constexpr int kExpectedPredicateCalls = EXPECT_STALE_CALLBACKS ? 0 : 1;

std::array<bool, 8> valid{false, false, true, true, false, true, true, true};
std::array<unsigned, 8> entered{}, predicateCalls{}, completed{};
std::array<uint32_t, 8> tuples{};
std::vector<int> order;
unsigned checks = 0u, failures = 0u;
unsigned invalidatedBeforeEntry = 0u, invalidatedStarted = 0u, resumedStarted = 0u;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
uint32_t word(const uint8_t* ram, uint32_t address) {
    uint32_t value = 0u;
    std::memcpy(&value, ram + address, sizeof(value));
    return value;
}
void checkCaller(const R5900Context& context) {
    check(getRegU32(&context, 29) == kCallerSp, "caller stack survives callback/cancellation");
    check(getRegU32(&context, 31) == kCallerRa, "caller return address survives callback/cancellation");
    check(getRegU32(&context, 16) == kCallerCookie, "caller saved register survives callback/cancellation");
}
GuestInvocation invocation(PS2Runtime* runtime, unsigned id, bool guarded = true) {
    GuestInvocation call{};
    call.kind = GuestInvocationKind::RpcCallback;
    call.context.pc = kCallback;
    SET_GPR_U32(&call.context, 4, id);
    tuples[id] = runtime->guestMalloc(32u, 16u);
    check(tuples[id] != 0u, "owned callback tuple allocates in real guest heap");
    const uint32_t marker = 0xc0de0000u | id;
    std::memcpy(runtime->memory().getRDRAM() + tuples[id], &marker, sizeof(marker));
    SET_GPR_U32(&call.context, 5, tuples[id]);
    if (guarded)
        call.shouldExecute = [id] { ++predicateCalls[id]; return valid[id]; };
    call.onComplete = [runtime, id](const R5900Context& result, R5900Context& caller) {
        ++completed[id];
        order.push_back(static_cast<int>(id) + 100);
        check(completed[id] == 1u, "tuple cleanup is invoked exactly once");
        check(result.pc == 0u, "cancelled and completed invocations share normal completion path");
        check(getRegU32(&result, 4) == id && getRegU32(&result, 5) == tuples[id],
              "callback argument/tuple ownership survives cancellation");
        check(getRegU32(&result, 29) != 0u, "callback receives scheduler-owned stack before completion");
        check(word(runtime->memory().getRDRAM(), tuples[id]) == (0xc0de0000u | id),
              "owned tuple remains alive until completion cleanup");
        if (id == 4u || id == 5u) {
            const unsigned next = id + 1u;
            check(getRegU32(&caller, 4) == next && getRegU32(&caller, 5) == tuples[next],
                  "sequence cleanup receives next attached callback context");
            check(getRegU32(&caller, 29) != 0u,
                  "next attached sequence callback retains its scheduler-owned stack");
        } else {
            checkCaller(caller);
        }
        runtime->guestFree(tuples[id]);
    };
    return call;
}
void startInvalidator(PS2Runtime* runtime, R5900Context* context, uint32_t pc, uint32_t stack) {
    EeThreadCreateParams parameters{};
    parameters.entry = pc;
    parameters.stack = stack;
    parameters.stackSize = 0x1000u;
    parameters.priority = 0;
    const int id = runtime->eeScheduler().createThread(parameters);
    check(id > EeScheduler::kMainThreadId, "real competing guest thread is created");
    check(runtime->eeScheduler().startThread(id, 0u, *context, false) == 0,
          "competing equal-priority guest thread becomes ready");
}
void boot(uint8_t*, R5900Context* context, PS2Runtime* runtime) {
    context->pc = kPreempt;
    runtime->eeScheduler().queueInvocation(invocation(runtime, 1u));
    runtime->eeScheduler().queueInvocation(invocation(runtime, 2u));
    runtime->eeScheduler().queueInvocation(invocation(runtime, 3u, false));
}
void callback(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    const unsigned id = getRegU32(context, 4);
    check(id > 0u && id < entered.size(), "callback id is valid");
    ++entered[id];
    order.push_back(static_cast<int>(id));
    check(word(ram, getRegU32(context, 5)) == (0xc0de0000u | id), "guest callback sees live tuple data");
    if (id == 1u || id == 4u || id == 6u || id == 7u)
        check(EXPECT_STALE_CALLBACKS != 0, "invalidated callback cannot enter guest code");
    if (id == 5u) {
        context->pc = kContinuation;
        ps2_syscalls::SleepThread(ram, context, runtime);
    }
    SET_GPR_U32(context, 2, id);
    context->pc = 0u;
}
void preempt(uint8_t*, R5900Context* context, PS2Runtime* runtime) {
    checkCaller(*context);
    const std::vector<int> expected = EXPECT_STALE_CALLBACKS
        ? std::vector<int>{1, 101, 2, 102, 3, 103}
        : std::vector<int>{101, 2, 102, 3, 103};
    check(order == expected, "queued cancellation preserves FIFO cleanup and valid/default callback order");
    check(predicateCalls[1] == kExpectedPredicateCalls && predicateCalls[2] == kExpectedPredicateCalls,
          "guarded queued calls evaluate once at execution");
    check(predicateCalls[3] == 0u, "default-empty predicate preserves unconditional callbacks");
    startInvalidator(runtime, context, kInvalidateBeforeEntry, 0x90000u);
    runtime->eeScheduler().queueInvocation(invocation(runtime, 7u));
    context->pc = kSequence;
    // Exhaust a real guest slice while the invalidator is ready. The dispatcher
    // attaches pending callback 7, then its normal pre-entry checkpoint switches
    // to the competing thread before shouldExecute or guest entry can occur.
    runtime->eeScheduler().accountCycles(static_cast<uint32_t>(EeScheduler::kDefaultTimeSliceCycles));
}
void invalidateBeforeEntry(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    const GuestThread* owner = runtime->eeScheduler().thread(EeScheduler::kMainThreadId);
    check(owner && owner->invocations.size() == 1u && owner->invocations.back().context.pc == kCallback,
          "real preemption occurs after callback attachment but before guest entry");
    check(entered[7] == 0u && predicateCalls[7] == 0u,
          "preempted attached invocation has not evaluated or executed");
    ++invalidatedBeforeEntry;
    valid[7] = false;
    order.push_back(207);
    ps2_syscalls::ExitThread(ram, context, runtime);
}
void sequence(uint8_t*, R5900Context* context, PS2Runtime* runtime) {
    checkCaller(*context);
    check(invalidatedBeforeEntry == 1u && completed[7] == 1u,
          "attached stale callback still cleans up after competing-thread invalidation");
    check(entered[7] == static_cast<unsigned>(EXPECT_STALE_CALLBACKS),
          "pre-entry invalidation suppresses guest body, while original control reproduces it");
    startInvalidator(runtime, context, kInvalidateStarted, 0x92000u);
    context->pc = kFinish;
    std::vector<GuestInvocation> calls;
    calls.push_back(invocation(runtime, 4u));
    calls.push_back(invocation(runtime, 5u));
    calls.push_back(invocation(runtime, 6u));
    runtime->eeScheduler().invokeCurrentSequence(std::move(calls));
}
void invalidateStarted(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    const GuestThread* owner = runtime->eeScheduler().thread(EeScheduler::kMainThreadId);
    check(owner && owner->status == EeThreadStatus::Waiting && owner->invocations.size() == 2u,
          "started callback suspends with later unstarted sequence entry retained");
    check(entered[5] == 1u && entered[6] == 0u,
          "first accepted sequence callback began before invalidation, later callback did not");
    check(predicateCalls[5] == kExpectedPredicateCalls && predicateCalls[6] == 0u,
          "only active sequence entry has evaluated its predicate");
    valid[5] = false;
    valid[6] = false;
    ++invalidatedStarted;
    order.push_back(205);
    check(runtime->eeScheduler().wakeupThread(EeScheduler::kMainThreadId, false) == 0,
          "actual kernel wake resumes the suspended callback");
    ps2_syscalls::ExitThread(ram, context, runtime);
}
void continuation(uint8_t*, R5900Context* context, PS2Runtime*) {
    ++resumedStarted;
    order.push_back(55);
    check(!valid[5] && invalidatedStarted == 1u, "callback became stale while suspended");
    check(predicateCalls[5] == kExpectedPredicateCalls,
          "started invocation predicate is never re-evaluated on continuation");
    check(getRegU32(context, 2) == 0u, "SleepThread completion result reaches started continuation");
    SET_GPR_U32(context, 2, 55u);
    context->pc = 0u;
}
void finish(uint8_t*, R5900Context* context, PS2Runtime* runtime) {
    checkCaller(*context);
    check(resumedStarted == 1u && entered[5] == 1u && completed[5] == 1u,
          "invalidation cannot cancel an already-started callback continuation");
    for (unsigned id = 1u; id < completed.size(); ++id) {
        check(completed[id] == 1u, "every queued/sequence callback releases its tuple once");
        check(predicateCalls[id] == (id == 3u ? 0u : static_cast<unsigned>(kExpectedPredicateCalls)),
              "all nonempty predicates execute once, including suspended sequence entries");
    }
    for (const unsigned id : {1u, 4u, 6u, 7u})
        check(entered[id] == static_cast<unsigned>(EXPECT_STALE_CALLBACKS),
              "only original control executes the four cancelled guest bodies");
    const auto index = [](int value) {
        for (size_t i = 0u; i < order.size(); ++i)
            if (order[i] == value) return i;
        return order.size();
    };
    check(index(104) < index(5) && index(5) < index(205) && index(205) < index(55) &&
          index(55) < index(105) && index(105) < index(106),
          "sequence cancellation and started suspension preserve completion ordering");
    const uint32_t reused = runtime->guestMalloc(32u, 16u);
    bool belongsToReleasedTuple = false;
    for (unsigned id = 1u; id < tuples.size(); ++id)
        belongsToReleasedTuple = belongsToReleasedTuple || reused == tuples[id];
    check(belongsToReleasedTuple, "completed/cancelled callback storage is reusable by actual guest heap");
    runtime->guestFree(reused);
    runtime->requestStop();
}
}
int main() {
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "actual guest memory initializes");
    runtime->registerFunction(kBoot, boot);
    runtime->registerFunction(kPreempt, preempt);
    runtime->registerFunction(kSequence, sequence);
    runtime->registerFunction(kFinish, finish);
    runtime->registerFunction(kCallback, callback);
    runtime->registerFunction(kContinuation, continuation);
    runtime->registerFunction(kInvalidateBeforeEntry, invalidateBeforeEntry);
    runtime->registerFunction(kInvalidateStarted, invalidateStarted);
    R5900Context context{};
    context.pc = kBoot;
    SET_GPR_U32(&context, 29, kCallerSp);
    SET_GPR_U32(&context, 31, kCallerRa);
    SET_GPR_U32(&context, 16, kCallerCookie);
    runtime->eeScheduler().reset(runtime->memory().getRDRAM(), context);
    runtime->eeScheduler().run();
    unsigned staleBodies = 0u;
    for (const unsigned id : {1u, 4u, 6u, 7u}) staleBodies += entered[id];
    std::printf("%s: %u checks, %u failures; stale bodies=%u, started resumes=%u\n",
                EXPECT_STALE_CALLBACKS ? "Original stale-callback control" : "Callback lifetime regression",
                checks, failures, staleBodies, resumedStarted);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
