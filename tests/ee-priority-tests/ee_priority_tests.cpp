#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "Syscalls/Helpers/State.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#ifndef EXPECT_ZERO_PRIORITY_REJECTION
#define EXPECT_ZERO_PRIORITY_REJECTION 0
#endif

namespace {
constexpr uint32_t kBoot = 0x00160000u;
constexpr uint32_t kCreateWorkers = kBoot + 0x10u;
constexpr uint32_t kRotate = kBoot + 0x20u;
constexpr uint32_t kAfterRotate = kBoot + 0x30u;
constexpr uint32_t kAfterHelperWake = kBoot + 0x40u;
constexpr uint32_t kFinal = kBoot + 0x50u;
constexpr uint32_t kHelper = kBoot + 0x100u;
constexpr uint32_t kHelperWake = kBoot + 0x110u;
constexpr uint32_t kWorker = kBoot + 0x200u;
constexpr uint32_t kWorkerWake = kBoot + 0x210u;
constexpr uint32_t kDescriptor = 0x00001800u;
constexpr uint32_t kStatus = 0x00001840u;

// PS2SDK t_ee_thread layout, written as an authentic guest RAM descriptor.
struct Descriptor {
    int32_t status;
    uint32_t entry, stack;
    int32_t stackSize;
    uint32_t gp;
    int32_t initialPriority, currentPriority;
    uint32_t attr, option;
};
static_assert(sizeof(Descriptor) == 0x24u);

size_t checks = 0u, failures = 0u;
int helperId = -1;
std::array<int, 2> workerIds{};
std::vector<int> order;
unsigned rotations = 0u;
unsigned workersRun = 0u;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
int result(const R5900Context* context) {
    return static_cast<int32_t>(getRegU32(context, 2));
}
int create(uint8_t* ram, R5900Context* context, PS2Runtime* runtime,
           int priority, uint32_t entry, uint32_t stack, uint32_t gp) {
    Descriptor descriptor{0, entry, stack, 0x1000, gp, priority, 99, 0x1234u, 0x5678u};
    std::memcpy(ram + kDescriptor, &descriptor, sizeof(descriptor));
    SET_GPR_U32(context, 4, kDescriptor);
    ps2_syscalls::CreateThread(ram, context, runtime);
    return result(context);
}
void start(uint8_t* ram, R5900Context* context, PS2Runtime* runtime, int id, uint32_t arg) {
    SET_GPR_U32(context, 4, static_cast<uint32_t>(id));
    SET_GPR_U32(context, 5, arg);
    ps2_syscalls::StartThread(ram, context, runtime);
    check(result(context) == 0, "actual StartThread succeeds");
}
void boot(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    EeScheduler& ee = runtime->eeScheduler();
    const GuestThread* main = ee.thread(EeScheduler::kMainThreadId);
    check(main && main->initialPriority == 0 && main->currentPriority == 0,
          "ExecPS2 bootstrap starts at priority zero");
    const size_t initialThreadCount = ee.snapshot().threads.size();
    check(create(ram, context, runtime, -1, kWorker, 0x20000u, 0u) == KE_ILLEGAL_PRIORITY,
          "negative CreateThread priority is rejected through actual guest ABI");
    check(create(ram, context, runtime, 128, kWorker, 0x20000u, 0u) == KE_ILLEGAL_PRIORITY,
          "priority 128 is rejected through actual guest ABI");
    check(ee.snapshot().threads.size() == initialThreadCount,
          "invalid priority descriptors do not allocate thread records");
    const int boundaryId = create(ram, context, runtime, 127, kWorker, 0x20000u, 0u);
    check(boundaryId > EeScheduler::kMainThreadId, "priority 127 remains valid");
    SET_GPR_U32(context, 4, static_cast<uint32_t>(boundaryId));
    ps2_syscalls::DeleteThread(ram, context, runtime);
    check(result(context) == 0, "dormant boundary thread can be deleted");

    helperId = create(ram, context, runtime, 0, kHelper, 0x22000u, 0x44440000u);
#if EXPECT_ZERO_PRIORITY_REJECTION
    check(helperId == KE_ILLEGAL_PRIORITY, "original scheduler reproduces rejected SDK priority-zero helper");
    check(ee.thread(EeScheduler::kMainThreadId)->currentPriority == 0,
          "failed retail InitThread sequence leaves main priority zero");
    context->pc = kCreateWorkers;
#else
    check(helperId > EeScheduler::kMainThreadId, "SDK priority-zero helper receives a separate thread id");
    const GuestThread* helper = ee.thread(helperId);
    check(helper && helper->status == EeThreadStatus::Dormant,
          "CreateThread creates a dormant SDK helper");
    check(helper && helper->initialPriority == 0 && helper->currentPriority == 0,
          "helper priority uses initial_priority, ignoring current_priority input");
    check(helper && helper->attr == 0x1234u && helper->option == 0x5678u,
          "guest descriptor metadata reaches actual scheduler");
    if (!helper) { runtime->requestStop(); return; }
    start(ram, context, runtime, helperId, 0xfeedu);
    check(ee.thread(helperId)->status == EeThreadStatus::Ready,
          "same-priority helper waits ready before main lowers its priority");
    // Exact retail SDK ordering: StartThread(helper), GetThreadId(), then
    // ChangeThreadPriority(caller, 1). There is no altered bootstrap default.
    context->pc = kCreateWorkers;
    ps2_syscalls::GetThreadId(ram, context, runtime);
    check(result(context) == EeScheduler::kMainThreadId, "actual GetThreadId identifies SDK caller");
    SET_GPR_U32(context, 4, static_cast<uint32_t>(result(context)));
    SET_GPR_U32(context, 5, 1u);
    ps2_syscalls::ChangeThreadPriority(ram, context, runtime);
#endif
}
void helper(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    order.push_back(100);
    check(runtime->eeScheduler().currentThreadId() == helperId, "priority-zero helper really executes");
    check(getRegU32(context, 4) == 0xfeedu, "helper receives StartThread argument");
    check(getRegU32(context, 28) == 0x44440000u && getRegU32(context, 29) == 0x23000u,
          "helper starts with authentic GP and stack descriptor");
    context->pc = kHelperWake;
    ps2_syscalls::SleepThread(ram, context, runtime);
}
void helperWake(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    check(result(context) == 0, "helper SleepThread resumes with successful wake result");
    order.push_back(101);
    context->pc = kHelperWake;
    ps2_syscalls::SleepThread(ram, context, runtime);
}
void createWorkers(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    EeScheduler& ee = runtime->eeScheduler();
#if !EXPECT_ZERO_PRIORITY_REJECTION
    check(ee.thread(EeScheduler::kMainThreadId)->currentPriority == 1,
          "retail ChangeThreadPriority naturally moved main to priority one");
    check(ee.thread(helperId)->status == EeThreadStatus::Waiting &&
          ee.thread(helperId)->wait.reason == EeWaitReason::Sleep,
          "highest-priority SDK helper sleeps instead of starving application threads");
    SET_GPR_U32(context, 4, static_cast<uint32_t>(helperId));
    SET_GPR_U32(context, 5, kStatus);
    ps2_syscalls::ReferThreadStatus(ram, context, runtime);
    check(result(context) == 0, "ReferThreadStatus supports priority-zero helper");
    int32_t initialPriority = -1, currentPriority = -1;
    std::memcpy(&initialPriority, ram + kStatus + 0x14u, 4u);
    std::memcpy(&currentPriority, ram + kStatus + 0x18u, 4u);
    check(initialPriority == 0 && currentPriority == 0, "guest-visible helper status preserves priority zero");
#endif
    for (unsigned index = 0u; index < workerIds.size(); ++index) {
        workerIds[index] = create(ram, context, runtime, 1, kWorker,
                                  0x24000u + index * 0x2000u, 0x55550000u + index);
        check(workerIds[index] > EeScheduler::kMainThreadId, "priority-one worker is created");
        start(ram, context, runtime, workerIds[index], index + 1u);
    }
    context->pc = kRotate;
}
void worker(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    const uint32_t arg = getRegU32(context, 4);
    check(arg >= 1u && arg <= 2u, "worker argument is valid");
    const unsigned index = arg == 2u ? 1u : 0u;
    check(runtime->eeScheduler().currentThreadId() == workerIds[index], "worker executes in its own kernel record");
    check(getRegU32(context, 28) == 0x55550000u + index, "worker has its descriptor GP");
    check(getRegU32(context, 29) == 0x25000u + index * 0x2000u, "worker has its descriptor stack top");
    SET_GPR_U32(context, 16, arg);
    order.push_back(static_cast<int>(arg));
    ++workersRun;
    context->pc = kWorkerWake;
    ps2_syscalls::SleepThread(ram, context, runtime);
}
void workerWake(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    check(result(context) == 0, "application worker resumes from actual WakeupThread");
    order.push_back(static_cast<int>(getRegU32(context, 16)) + 10);
    ps2_syscalls::ExitThread(ram, context, runtime);
}
void rotate(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    ++rotations;
    context->pc = kAfterRotate;
    SET_GPR_U32(context, 4, 1u);
    ps2_syscalls::RotateThreadReadyQueue(ram, context, runtime);
}
void afterRotate(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
#if EXPECT_ZERO_PRIORITY_REJECTION
    if (rotations < 128u) { context->pc = kRotate; return; }
    check(workersRun == 0u, "128 retail RotateThreadReadyQueue(1) calls cannot yield a priority-zero main");
    for (const int id : workerIds)
        check(runtime->eeScheduler().thread(id)->status == EeThreadStatus::Ready,
              "original failure leaves movie-like workers ready but starved");
    runtime->requestStop();
#else
    check(workersRun == 2u && order == std::vector<int>({100, 1, 2}),
          "same-priority rotation runs both workers in FIFO order before returning to main");
    for (const int id : workerIds)
        check(runtime->eeScheduler().thread(id)->status == EeThreadStatus::Waiting,
              "both application workers reached SleepThread");
    context->pc = kAfterHelperWake;
    SET_GPR_U32(context, 4, static_cast<uint32_t>(helperId));
    ps2_syscalls::WakeupThread(ram, context, runtime);
#endif
}
void afterHelperWake(uint8_t* ram, R5900Context* context, PS2Runtime* runtime) {
    check(order == std::vector<int>({100, 1, 2, 101}),
          "waking priority-zero helper preempts main and completes before its continuation");
    for (const int id : workerIds) {
        SET_GPR_U32(context, 4, static_cast<uint32_t>(id));
        ps2_syscalls::WakeupThread(ram, context, runtime);
        check(result(context) == 0, "main wakes sleeping equal-priority application worker");
    }
    context->pc = kFinal;
    SET_GPR_U32(context, 4, 1u);
    ps2_syscalls::RotateThreadReadyQueue(ram, context, runtime);
}
void final(uint8_t*, R5900Context*, PS2Runtime* runtime) {
    check(order == std::vector<int>({100, 1, 2, 101, 11, 12}),
          "both worker wake continuations execute once in FIFO order");
    for (const int id : workerIds)
        check(runtime->eeScheduler().thread(id)->status == EeThreadStatus::Dormant,
              "worker ExitThread leaves a dormant reusable thread record");
    check(runtime->eeScheduler().thread(helperId)->status == EeThreadStatus::Waiting,
          "SDK helper returns to sleep after dispatching work");
    check(runtime->eeScheduler().thread(EeScheduler::kMainThreadId)->currentPriority == 1,
          "bootstrap remains changed by retail InitThread sequence only");
    runtime->requestStop();
}
}

int main() {
    auto runtime = std::make_unique<PS2Runtime>();
    check(runtime->memory().initialize(), "actual runtime memory initializes");
    runtime->registerFunction(kBoot, boot);
    runtime->registerFunction(kCreateWorkers, createWorkers);
    runtime->registerFunction(kRotate, rotate);
    runtime->registerFunction(kAfterRotate, afterRotate);
    runtime->registerFunction(kAfterHelperWake, afterHelperWake);
    runtime->registerFunction(kFinal, final);
    runtime->registerFunction(kHelper, helper);
    runtime->registerFunction(kHelperWake, helperWake);
    runtime->registerFunction(kWorker, worker);
    runtime->registerFunction(kWorkerWake, workerWake);
    R5900Context context{};
    context.pc = kBoot;
    SET_GPR_U32(&context, 29, 0x30000u);
    runtime->eeScheduler().reset(runtime->memory().getRDRAM(), context);
    runtime->eeScheduler().run();
    std::printf("%s: %zu checks, %zu failures, helper=%d, workers=%u, rotations=%u\n",
                EXPECT_ZERO_PRIORITY_REJECTION ? "Original starvation control" : "Priority-zero SDK regression",
                checks, failures, helperId, workersRun, rotations);
    return failures == 0u ? EXIT_SUCCESS : EXIT_FAILURE;
}
