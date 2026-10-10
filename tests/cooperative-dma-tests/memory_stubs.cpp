// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
// Rasterization is unrelated to this parser/DMA fixture. Actual memory,
// transfer snapshots, VIF decoding and GIF arbitration are linked.
GS::GS() = default;
void GS::processGIFPacket(const uint8_t *, uint32_t) {}
bool GS::processNativePackedGIFPacket(const uint8_t *, uint32_t) { return false; }
void GS::uploadImageNative(uint64_t,uint64_t,uint64_t,uint64_t,const uint8_t *,uint32_t) {}
