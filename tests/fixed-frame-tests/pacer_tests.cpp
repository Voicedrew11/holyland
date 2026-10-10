// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/fixed_frame_pacer.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace std::chrono;
static void require(bool condition, const char *message)
{
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
int main()
{
    const auto epoch = FixedFramePacer::TimePoint{} + seconds(5);
    FixedFramePacer pacing;
    require(pacing.next(epoch) == epoch, "First completed update must not wait an extra frame");
    auto previous = epoch;
    for (int n = 1; n <= 30000; ++n)
    {
        // Early completed work waits for its absolute deadline; no per-frame
        // truncation error accumulates over 1,001 seconds.
        auto next = pacing.next(previous + milliseconds(1));
        const auto exact = epoch + nanoseconds(int64_t(n) * 1001000000000LL / 30000);
        require(next == exact, "NTSC frame deadline drifted");
        require(next > previous + milliseconds(33), "Simulation exceeded native rate");
        previous = next;
    }
    // A pause or loading stall must discard elapsed backlog, not release a
    // stream of instant updates that speeds up physics to catch the host clock.
    auto resumed = pacing.next(previous + seconds(8));
    require(resumed == previous + seconds(8), "Long pause did not rebase");
    auto after = pacing.next(resumed + milliseconds(1));
    require(after > resumed + milliseconds(33), "Pause caused catch-up burst");
    FixedFramePacer second;
    require(second.next(epoch) == epoch, "Independent pacing state contaminated");
    auto late = second.next(epoch + milliseconds(36));
    require(late == epoch + nanoseconds(33366666), "Small overrun must retain the fixed timeline");
    auto recovered = second.next(epoch + milliseconds(60));
    require(recovered == epoch + nanoseconds(66733333), "Early next update must recover transient lateness");
    std::puts("Fixed frame pacing passed: rational cadence, slow frames, pause recovery, independent clocks");
}
