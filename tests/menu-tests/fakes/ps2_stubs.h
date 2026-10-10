#pragma once

#include "ps2_runtime.h"

#define PS2_STUB_LIST(X) X(sceSifFreeIopHeap)

namespace ps2_stubs
{
    void sceSifFreeIopHeap(uint8_t *, R5900Context *, PS2Runtime *);
}
