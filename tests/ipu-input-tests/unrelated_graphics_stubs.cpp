#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

// These graphics paths are not called by the IPU-input DMA tests. The actual
// PS2Memory MMIO, RAM, DMA-register and interrupt implementation is linked.
void PS2Memory::processVIF0Data(uint32_t, uint32_t) {}
void PS2Memory::processVIF0Data(const uint8_t *, uint32_t) {}
void PS2Memory::processVIF1Data(uint32_t, uint32_t) {}
void PS2Memory::processVIF1Data(const uint8_t *, uint32_t) {}
void GifArbiter::submit(GifPathId, const uint8_t *, uint32_t, bool) {}
void GifArbiter::drain() {}
bool GS::processNativePackedGIFPacket(const uint8_t *, uint32_t) { return false; }
void GS::uploadImageNative(uint64_t, uint64_t, uint64_t, uint64_t, const uint8_t *, uint32_t) {}
