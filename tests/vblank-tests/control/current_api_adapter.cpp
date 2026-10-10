// SPDX-License-Identifier: GPL-3.0-or-later
// Interface compatibility only for the pinned original-scheduler negative
// control. It deliberately has neither wall-clock MMIO synchronization nor
// video-mode pacing; positive fixtures use the real rebuilt runtime methods.
#include "runtime/ee_scheduler.h"

void EeScheduler::synchronizeTimerClock() noexcept {}
void EeScheduler::setGsVideoMode(uint32_t, uint32_t) {}
