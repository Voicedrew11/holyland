// The PC renderer's GPU time by kind of work (docs/port/PC_RENDERER.md, handoff item 1): a timestamp wherever the kind
// of work recorded into a command buffer changes, each interval added to the kind being recorded in it. On with
// KF4_GSN_GPU_TIME=1 (the switch the native GS reads, which tools/pace_test.py sets: its "GPU ms a picture") and while
// the overlay's profiler is open (Shift+P: PROFILE_GPU); off, nothing is written.
//
// Intervals never span two command buffers, so the time between submits (the GPU idle) is not counted.
//
// One thread: the PC renderer's ("kf4-pc").
#pragma once

#include <cstdint>
#include <memory>

namespace Vulkan {
class CommandBuffer;
class Device;
} // namespace Vulkan

namespace pcr {

// What the GPU is doing between two timestamps
enum class GpuWork : uint8_t {
    draws_2d,     // the GIF's primitives (sprites, triangles, strips)
    native_000,   // native batches by their VU1 program
    native_280,
    native_700,
    date_copy,    // the surface copied for DATE
    texture_copy, // a texture read from a surface: the surface copied
    surface,      // surfaces made, grown, cleared
    upload,       // uploads written into surfaces
    copy,         // local copies between surfaces, read-backs
    picture,      // the picture: the scanout, its copy into the ring, read back
    count,
};

class GpuTimes {
public:
    explicit GpuTimes(Vulkan::Device &device);
    ~GpuTimes();
    GpuTimes(const GpuTimes &) = delete;
    GpuTimes &operator=(const GpuTimes &) = delete;

    // What cmd records from here on is work's, until the next begin or end (a begin in another command buffer ends
    // the open one's interval first)
    void begin(Vulkan::CommandBuffer &cmd, GpuWork work);
    // cmd about to be submitted: its open interval closed (no-op if cmd has none). Every submit is preceded by it.
    void end(Vulkan::CommandBuffer &cmd);

    // A picture made: the count the time a picture is divided by
    void picture();
    // The intervals whose timestamps are in, added to their kinds (and to the profiler's PROFILE_GPU): each vblank
    void collect();

    // --stats: "[stats] pc renderer GPU time: T ms in all, P ms a picture (N pictures): <kind> <ms a picture> ...",
    // the shape pace_test reads. Waits for the GPU first. Nothing when it was never on.
    void print();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
