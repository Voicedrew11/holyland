// The scene renderer's drawing side (scn_scene.h): a closed scene's steps recorded into a command buffer, in order.
// Its programs are the PC renderer's shaders built without exact Z (pcr_prim.vert and .frag, pcr_nd000.vert,
// pcr_nd280.vert, pcr_nd700.vert, pcr_ndripple.vert), with the depth the GPU's: each draw's Z / zmax, clamped at each
// pixel by the depth clamp to the viewport's 0 to 1, into D16_UNORM for the game's 16-bit Z buffers (the GS's whole Z,
// rounded by the GPU) and D32F for the others (docs/plans/scene-renderer-design.md, "Depth's exactness and DATE"). And
// DATE's shadows by the first-draw image: scn_first_draw.frag, then pcr_prim.frag with SCN_FIRST_DRAW.
// One thread: the builder's.
#pragma once

#include <stdlib.h>

#include <array>
#include <cstdint>

#include "device.hpp"
#include "pcr_gpu_time.h"
#include "scn_scene.h"

namespace scn {

class Drawer {
public:
    Drawer(Vulkan::Device &device, pcr::GpuTimes &times);
    Drawer(const Drawer &) = delete;
    Drawer &operator=(const Drawer &) = delete;

    // The scene's steps into cmd, in order: each op between full barriers, each pass a render pass. What follows in cmd
    // sees all of it.
    void record(const Scene &scene, Vulkan::CommandBuffer &cmd);

    // DATE by the first-draw image is possible here: an R32F colour target that blends (MIN)
    bool first_draw_ok() const {
        return first_draw_ok_;
    }
    static constexpr VkFormat first_format = VK_FORMAT_R32_SFLOAT;

    // Vulkan's dynamic state for the draws, as the canvas has it (pcr_canvas.cpp's choose_dynamic): set on every
    // command buffer the scene is recorded into
    bool dynamic_ds() const {
        return dynamic_ds_;
    }
    bool dynamic_blend() const {
        return dynamic_blend_;
    }

    struct Counts {
        uint64_t passes = 0, draws = 0, native_draws = 0, gif_draws = 0, clears = 0, ops = 0;
        uint64_t first_draws = 0, darkened = 0; // (DATE's shadows: drawn into the first-draw image, then darkened)
        // (draws that set their material's state: the others are the one before's)
        uint64_t state_set = 0;
    };
    const Counts &counts() const {
        return counts_;
    }

private:
    void record_op(const Op &op, Vulkan::CommandBuffer &c);
    void record_pass(const Scene &scene, const Pass &pass, Vulkan::CommandBuffer &c);
    // set: the material whose state the command buffer has from the draw before in the pass (no_material: none)
    void record_draw(const Scene &scene, const Pass &pass, const Draw &d, Vulkan::CommandBuffer &c, uint32_t set);
    void full_barrier(Vulkan::CommandBuffer &c);
    void pass_to_pass_barrier(Vulkan::CommandBuffer &c);
    void make_programs();

    Vulkan::Device &device_;
    pcr::GpuTimes &times_;
    bool dynamic_ds_ = false, dynamic_blend_ = false, first_draw_ok_ = false;
    const bool time_clears_ = getenv("KF4_SCN_TIME_CLEARS") && atoi(getenv("KF4_SCN_TIME_CLEARS")) != 0;
    // by Program: the plain fragment shader, the one tested before it (pcr_prim_early.frag), the first-draw image's
    // (scn_first_draw.frag) and the shadows' darkening (pcr_prim.frag with SCN_FIRST_DRAW)
    std::array<Vulkan::Program *, size_t(Program::count)> plain_{}, early_{}, first_id_{}, first_{};
    Vulkan::Program *picture_layer_ = nullptr; // (pcr_prim.vert with scn_picture_layer.frag)
    Vulkan::ImageHandle dummy_uint_, dummy_float_;
    Vulkan::BufferHandle zeros_, no_clut_, no_trace_;
    Vulkan::BindlessDescriptorPoolHandle table_pool_;
    Vulkan::BindlessDescriptorSet table_{};
    Counts counts_;
};

} // namespace scn
