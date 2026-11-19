// The PC renderer's plane pass (exact Z, shaders/pcr_exact_z.h): the planes of the native draws' triangles, worked out
// once a triangle by a compute shader a native program (shaders/pcr_planes_*.comp) before the draws that read them.
//
// The canvas reserves room for a native batch's triangles as it records the batch's draws, and writes an entry a
// triangle there (which triangle of the program's, its draw's instance, the frame's place and the target's scale).
// Just before the canvas submits its command buffer, submit() records the pass for every entry since the last submit
// in a command buffer of its own and submits it first, ending with a barrier before the vertex shaders' reads: the
// canvas's draws then read their triangles' planes (pcr_exact_z.h's pcr_plane_of_draw), each at the entry's number in
// the planes buffer its room names.
//
// Room is in chunks (an entries buffer the CPU writes, a planes buffer the GPU writes): a batch's triangles are in one
// chunk, and a chunk lives until the submit after it is filled (Granite's deferred deletion keeps it until the GPU is
// done). One thread: the canvas's.
#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "device.hpp"

namespace pcr {

class GpuTimes;

// A triangle's entry (shaders/pcr_planes.h's)
struct PlaneEntry {
    uint32_t triangle; // its triangle in the program's triangles
    uint32_t instance; // its draw's instance (gl_InstanceIndex to the program's vertex shader)
    uint32_t place;    // the frame's place in its surface, GS pixels: X bits 0-15, Y 16-31
    uint32_t scale;    // the target's scale: X bits 0-7, Y 8-15
};

// The buffers a native program's plane pass reads: its vertex shader's (the canvas's bind_between)
struct PlaneSources {
    uint32_t program;                 // the canvas's native slot: 0 program 0x000, 1 0x280, 2 0x700, 3 the ripple
    const Vulkan::Buffer *picture;    // a picture between's picture, or the canvas's zeros
    Vulkan::BufferHandle arena, records, triangles;
};

// Room for a batch's triangles
struct PlaneRoom {
    PlaneEntry *entries = nullptr;          // count of them, for the caller to write; nullptr: no room (no planes)
    const Vulkan::Buffer *planes = nullptr; // the planes buffer the draws read (set 1, binding 9)
    uint32_t first = 0;                     // the first entry's number in it
    bool new_chunk = false;                 // the planes buffer is not the last room's (a run bound to it ends first)
};

class PlanePass {
public:
    explicit PlanePass(Vulkan::Device &device);
    ~PlanePass();
    PlanePass(const PlanePass &) = delete;
    PlanePass &operator=(const PlanePass &) = delete;

    // These dispatches use the default compute pipeline state and no specialization constants.
    std::span<Vulkan::Program *const> compute_programs() const;

    // Room for count triangles read from sources
    PlaneRoom reserve(const PlaneSources &sources, uint32_t count);
    // The canvas's command buffer about to be submitted: the pass for the entries since the last submit recorded and
    // submitted before it (nothing when there are none). Timed as GpuWork::planes. Returns what it submitted.
    struct Submitted {
        uint32_t triangles = 0, dispatches = 0;
    };
    Submitted submit(GpuTimes &times);

    // --stats: triangles worked out, dispatches
    uint64_t triangles() const;
    uint64_t dispatches() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pcr
