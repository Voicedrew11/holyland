#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

// The actual PS2Memory allocator, VU code tracking and PATH1 callback run.
// Window creation, GS rasterization and unrelated VIF channels do not run.
GS::GS() = default;
void GS::processGIFPacket(const uint8_t *, uint32_t) {}
void PS2Memory::processVIF0Data(uint32_t, uint32_t) {}
void PS2Memory::processVIF0Data(const uint8_t *, uint32_t) {}
void PS2Memory::processVIF1Data(uint32_t, uint32_t) {}
void PS2Memory::processVIF1Data(const uint8_t *, uint32_t) {}
bool PS2Memory::vif1WorkPending() const noexcept { return false; }
void PS2Memory::cancelVif1Work() {}
void GifArbiter::submit(GifPathId, const uint8_t *, uint32_t, bool) {}
void GifArbiter::drain() {}
bool GS::processNativePackedGIFPacket(const uint8_t *, uint32_t) { return false; }
void GS::uploadImageNative(uint64_t, uint64_t, uint64_t, uint64_t, const uint8_t *, uint32_t) {}
