// The PC renderer's GPU time by kind of work (docs/port/PC_RENDERER.md, handoff item 1): a timestamp wherever the kind
// of work recorded into a command buffer changes, each interval added to the kind being recorded in it. On with
// KF4_GSN_GPU_TIME=1 (the switch the native GS reads, which tools/pace_test.py sets: its "GPU ms a picture") and while
// the overlay's profiler is open (Shift+P: PROFILE_GPU); off, nothing is written.
//
// Intervals never span two command buffers, so the time between submits (the GPU idle) is not counted.
//
// While a profile run records (port/include/port/profile_run.h) each interval is also added to the profiler's GPU
// table (port_profile_gpu_add): by kind, and with its detail on (port_profile_gpu_detail), by shader state too, a
// timestamp at each change of state. KF4_GPU_LABELS=1 also marks each interval in the command buffer as a debug label
// (VK_EXT_debug_utils) named for its kind, for Radeon GPU Profiler, RenderDoc or Nsight.
//
// One thread each: the PC renderer's ("kf4-pc") and the pump's ("kf4-pc-pump") have one each, put together for --stats.
#pragma once

#include <cstddef>
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
    date_copy,    // the surface copied for DATE (KF4_PC_STENCIL=0)
    blend_copy,   // the rectangle a blend with no Vulkan form draws in, copied
    texture_copy, // a texture read from a surface: the surface copied
    surface,      // surfaces made, grown, cleared
    upload,       // uploads written into surfaces
    copy,         // local copies between surfaces, read-backs
    picture,      // the picture: the scanout, its copy into the ring, read back
    count,
};

// A draw's shader state, as the GPU's time is told by it (port_profile_gpu_detail): what changes the work its shader
// does. 0 is none (work that is not a draw).
struct ShaderState {
    uint8_t program;      // 0: 2D; 1, 2, 3: native 000, 280, 700
    uint8_t texture;      // 0: none; 1: texels; 2: a surface's copy; 3: sampled (filtered) texture
    uint8_t tfx;          // the texture function (modulate, decal, highlight, highlight2)
    bool alpha_channel;   // TCC
    bool fog;
    uint8_t blend;        // ALPHA's A, B, C, D (2 bits each), when blending
    bool blending;
    bool shader_blend;    // the GS's blend done in the shader, from a copy
    uint8_t date;         // 0: none; 1: by the stencil; 2: from a copy
    uint8_t depth;        // 0: no Z test; else ZTST (1 always, 2 greater or equal, 3 greater)
    bool depth_write;
    uint8_t passes;       // the alpha test's passes (1 or 2)
};
// The state in 32 bits (never 0), and back as text for the profiler's row
uint32_t shader_state_key(const ShaderState &state);
void shader_state_text(uint32_t key, char *text, size_t size);

class GpuTimes {
public:
    explicit GpuTimes(Vulkan::Device &device);
    ~GpuTimes();
    GpuTimes(const GpuTimes &) = delete;
    GpuTimes &operator=(const GpuTimes &) = delete;

    // What cmd records from here on is work's, until the next begin or end (a begin in another command buffer ends
    // the open one's interval first)
    void begin(Vulkan::CommandBuffer &cmd, GpuWork work);
    // The same for a draw of shader state key (shader_state_key): with the profiler's detail on, a change of state
    // begins an interval of its own
    void begin_draw(Vulkan::CommandBuffer &cmd, GpuWork work, uint32_t state_key);
    // cmd about to be submitted: its open interval closed (no-op if cmd has none). Every submit is preceded by it.
    void end(Vulkan::CommandBuffer &cmd);

    // A picture made: the count the time a picture is divided by
    void picture();
    // The intervals whose timestamps are in, added to their kinds (and to the profiler's PROFILE_GPU): each vblank
    void collect();

    // (both threads idle) other's time and pictures added to this one's, other left empty: the pump's into kf4-pc's
    void take_in(GpuTimes &other);

    // --stats: "[stats] pc renderer GPU time: T ms in all, P ms a picture (N pictures): <kind> <ms a picture> ...",
    // the shape pace_test reads. Waits for the GPU first. Nothing when it was never on.
    void print();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
