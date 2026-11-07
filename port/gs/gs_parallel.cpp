// The GS on the PC: paraLLEl-GS (third_party/parallel-gs), the GS rasteriser in Vulkan compute, behind
// the small C interface port/platform/gs.c calls (port/include/port/platform.h, "the GS backend").
// All of it runs on the DMA controller's thread, which is the only one that talks to the GS.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"

extern "C" {
#include "port/platform.h"
#include "port/savestate.h"
}

using namespace ParallelGS;
using namespace Vulkan;

namespace {
Context *context;
Device *device;
GSInterface *gs;
BufferHandle readback;
VkDeviceSize readback_size;
}

extern "C" int port_gs_backend_open(void) {
    if (!Context::init_loader(nullptr)) {
        fprintf(stderr, "[gs] no Vulkan loader\n");
        return 0;
    }
    context = new Context;
    context->set_num_thread_indices(1);
    if (!context->init_instance_and_device(nullptr, 0, nullptr, 0,
                                           CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                               CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                               CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT)) {
        fprintf(stderr, "[gs] no Vulkan device that paraLLEl-GS can use\n");
        delete context;
        context = nullptr;
        return 0;
    }
    device = new Device;
    device->set_context(*context);
    device->init_frame_contexts(4);

    gs = new GSInterface;
    GSOptions options = {};
    if (!gs->init(device, options)) {
        fprintf(stderr, "[gs] paraLLEl-GS did not start\n");
        return 0;
    }
    // the same packets give the same pictures
    DebugMode debug = {};
    debug.deterministic_timeline_query = true;
    gs->set_debug_mode(debug);
    fprintf(stderr, "[gs] paraLLEl-GS on %s\n", device->get_gpu_properties().deviceName);
    return 1;
}

// The Vulkan context paraLLEl-GS draws on (--renderer gs)
extern "C" void *port_gs_backend_context(void) {
    return context;
}

extern "C" void port_gs_backend_gif(int path, const void *data, size_t bytes) {
    gs->gif_transfer(uint32_t(path), data, bytes);
}

extern "C" void port_gs_backend_read_fifo(void *dst, uint32_t qwords) {
    gs->read_transfer_fifo(dst, qwords);
}

extern "C" int port_gs_backend_vsync(const uint64_t *privileged, int field, int want_pixels,
                                     PortGsFrame *frame) {
    auto &priv = gs->get_priv_register_state();
    // PMODE..BGCOLOR, each 64-bit register at a 16-byte stride, as in the register page
    memcpy(priv.qwords_lo, privileged, 0xF0);

    VSyncInfo info = {};
    info.phase = uint32_t(field);
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_COPY_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;
    // the PC's ways from the GS's raw output, each of which can be turned off to compare (main_host.c)
    info.force_progressive = port_options.progressive != 0;
    info.anti_blur = port_options.anti_blur != 0;
    info.adapt_to_internal_horizontal_resolution = port_options.adapt_width != 0;
    info.skip_deinterlace = !port_options.deinterlace;

    gs->flush();
    ScanoutResult result = gs->vsync(info);
    if (!want_pixels || !result.image) {
        return 0;
    }

    uint32_t width = result.image->get_width();
    uint32_t height = result.image->get_height();
    if (width * height > PORT_GS_FRAME_PIXELS) {
        return 0;
    }
    VkDeviceSize size = VkDeviceSize(width) * height * 4;
    if (!readback || readback_size < size) {
        BufferCreateInfo buffer_info = {};
        buffer_info.domain = BufferDomain::CachedHost;
        buffer_info.size = size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        readback = device->create_buffer(buffer_info, nullptr);
        readback_size = size;
    }

    auto cmd = device->request_command_buffer();
    cmd->copy_image_to_buffer(*readback, *result.image, 0, {0, 0, 0}, {width, height, 1}, 0, 0,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                 VK_ACCESS_HOST_READ_BIT);
    Fence fence;
    device->submit(cmd, &fence);
    fence->wait();

    auto *pixels = static_cast<const uint8_t *>(device->map_host_buffer(*readback, MEMORY_ACCESS_READ_BIT));
    bool bgra = result.image->get_format() == VK_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t i = 0; i < width * height; i++) {
        const uint8_t *p = pixels + i * 4;
        uint8_t r = bgra ? p[2] : p[0], b = bgra ? p[0] : p[2];
        // RGBA in memory order; the scanout's alpha means nothing on a screen
        frame->rgba[i] = uint32_t(r) | (uint32_t(p[1]) << 8) | (uint32_t(b) << 16) | 0xFF000000u;
    }
    device->unmap_host_buffer(*readback, MEMORY_ACCESS_READ_BIT);
    frame->width = width;
    frame->height = height;
    return 1;
}

// ---- Save states (port/platform/gs.c) -------------------------------------------------------------
// VRAM and the registers, taken between GIF packets, so no path is inside one and no transfer waits
// for its pixels. A load starts from the GS's reset state (paths, vertex queue), then puts back VRAM
// and the registers and has paraLLEl-GS take them all as new.
//
// And what the scanout keeps from the last pictures, without which the first four after a load would
// differ from the run that saved: the last four fields, which the deinterlacer weaves each picture
// from (GSRenderer::vsync_last_fields), and the countdown that turns --no-progressive's way on for four
// vblanks after a complex SCANMSK (has_complex_scanmsk_timeout). Both are private: the functions
// below reach them through pointers to members named in an explicit template instantiation, where
// C++ lets private names be used.
static const size_t vram_bytes = 4 * 1024 * 1024; // GSOptions' vram_size, the GS's 4 MB
static const int history_fields = 4;

ImageHandle *gs_history(GSInterface &g);
uint32_t &gs_scanmsk_timeout(GSInterface &g);

template <typename RendererMember, RendererMember renderer_member, typename FieldsMember, FieldsMember fields_member,
          typename TrackerMember, TrackerMember tracker_member>
struct GSPrivate {
    friend ImageHandle *gs_history(GSInterface &g) {
        return (g.*renderer_member).*fields_member;
    }
    friend uint32_t &gs_scanmsk_timeout(GSInterface &g) {
        return (g.*tracker_member).has_complex_scanmsk_timeout;
    }
};
template struct GSPrivate<decltype(&GSInterface::renderer), &GSInterface::renderer,
                          decltype(&GSRenderer::vsync_last_fields), &GSRenderer::vsync_last_fields,
                          decltype(&GSInterface::state_tracker), &GSInterface::state_tracker>;

static ImageHandle *history(void) {
    return gs_history(*gs);
}

// A field: the one before it that is the same image (or -1), its size, then (when new) its pixels
struct FieldHeader {
    int32_t same_as;
    uint32_t width;
    uint32_t height;
    uint32_t present;
};

static FieldHeader field_header(const ImageHandle *fields, int i) {
    FieldHeader h = {-1, 0, 0, 0};

    if (!fields[i]) {
        return h;
    }
    h.present = 1;
    for (int j = 0; j < i; j++) {
        if (fields[j] == fields[i]) {
            h.same_as = j;
        }
    }
    h.width = fields[i]->get_width();
    h.height = fields[i]->get_height();
    return h;
}

static void save_field(PortStateIo *io, const Image &image) {
    VkDeviceSize size = VkDeviceSize(image.get_width()) * image.get_height() * 4;
    BufferCreateInfo buffer_info = {};
    buffer_info.domain = BufferDomain::CachedHost;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BufferHandle buffer = device->create_buffer(buffer_info, nullptr);

    auto cmd = device->request_command_buffer();
    cmd->image_barrier(image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
    cmd->copy_image_to_buffer(*buffer, image, 0, {0, 0, 0}, {image.get_width(), image.get_height(), 1}, 0, 0,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    cmd->image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                 VK_ACCESS_HOST_READ_BIT);
    Fence fence;
    device->submit(cmd, &fence);
    fence->wait();
    void *pixels = device->map_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
    port_state_bytes(io, pixels, size_t(size));
    device->unmap_host_buffer(*buffer, MEMORY_ACCESS_READ_BIT);
}

static ImageHandle load_field(PortStateIo *io, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    port_state_bytes(io, pixels.data(), pixels.size());

    auto image_info = ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.misc |= IMAGE_MISC_MUTABLE_SRGB_BIT;
    ImageInitialData initial = {pixels.data(), 0, 0};
    return device->create_image(image_info, &initial);
}

extern "C" size_t port_gs_backend_state_size(void) {
    const ImageHandle *fields = history();
    size_t size = vram_bytes + sizeof(RegisterState) + sizeof(PrivRegisterState) + sizeof(uint32_t);

    for (int i = 0; i < history_fields; i++) {
        FieldHeader h = field_header(fields, i);

        size += sizeof(h);
        if (h.present && h.same_as < 0) {
            size += size_t(h.width) * h.height * 4;
        }
    }
    return size;
}

extern "C" void port_gs_backend_state(PortStateIo *io) {
    ImageHandle *fields = history();

    if (!io->loading) {
        gs->flush();
        port_state_bytes(io, const_cast<void *>(gs->map_vram_read(0, vram_bytes)), vram_bytes);
        RegisterState registers = gs->get_register_state();
        PrivRegisterState priv = gs->get_priv_register_state();
        port_state_bytes(io, &registers, sizeof(registers));
        port_state_bytes(io, &priv, sizeof(priv));
        port_state_bytes(io, &gs_scanmsk_timeout(*gs), sizeof(uint32_t));
        for (int i = 0; i < history_fields; i++) {
            FieldHeader h = field_header(fields, i);

            port_state_bytes(io, &h, sizeof(h));
            if (h.present && h.same_as < 0) {
                save_field(io, *fields[i]);
            }
        }
        return;
    }
    gs->reset_context_state();
    port_state_bytes(io, gs->map_vram_write(0, vram_bytes), vram_bytes);
    gs->end_vram_write(0, vram_bytes);
    port_state_bytes(io, &gs->get_register_state(), sizeof(RegisterState));
    port_state_bytes(io, &gs->get_priv_register_state(), sizeof(PrivRegisterState));
    gs->clobber_register_state();
    gs->flush();
    port_state_bytes(io, &gs_scanmsk_timeout(*gs), sizeof(uint32_t));
    for (int i = 0; i < history_fields; i++) {
        FieldHeader h;

        port_state_bytes(io, &h, sizeof(h));
        if (!h.present) {
            fields[i].reset();
        } else if (h.same_as >= 0 && h.same_as < i) {
            fields[i] = fields[h.same_as];
        } else {
            fields[i] = load_field(io, h.width, h.height);
        }
    }
}

// ---- The debugging tools' GS capture (port/debug/gs_capture.c, tools/gsreplay) --------------------
// The same state as a save state's, in the capture file's build-independent records
// (port/include/port/gs_capture_format.h): VRAM as the GS lays it out, the registers by their GS
// address, the privileged registers by theirs, and the scanout's history as plain pictures.
extern "C" {
#include "port/debug.h"
}

namespace {
struct RegSlot {
    uint32_t address;
    uint64_t *bits;
};

std::vector<RegSlot> register_slots(RegisterState &r) {
    std::vector<RegSlot> slots;
    for (uint32_t c = 0; c < 2; c++) {
        ContextState &ctx = r.ctx[c];
        slots.push_back({0x06 + c, &ctx.tex0.bits});
        slots.push_back({0x14 + c, &ctx.tex1.bits});
        slots.push_back({0x08 + c, &ctx.clamp.bits});
        slots.push_back({0x18 + c, &ctx.xyoffset.bits});
        slots.push_back({0x34 + c, &ctx.miptbl_1_3.bits});
        slots.push_back({0x36 + c, &ctx.miptbl_4_6.bits});
        slots.push_back({0x40 + c, &ctx.scissor.bits});
        slots.push_back({0x42 + c, &ctx.alpha.bits});
        slots.push_back({0x47 + c, &ctx.test.bits});
        slots.push_back({0x4A + c, &ctx.fba.bits});
        slots.push_back({0x4C + c, &ctx.frame.bits});
        slots.push_back({0x4E + c, &ctx.zbuf.bits});
    }
    slots.push_back({0x00, &r.prim.bits});
    slots.push_back({0x01, &r.rgbaq.bits});
    slots.push_back({0x02, &r.st.bits});
    slots.push_back({0x03, &r.uv.bits});
    slots.push_back({0x0A, &r.fog.bits});
    slots.push_back({0x1A, &r.prmodecont.bits});
    slots.push_back({0x1C, &r.texclut.bits});
    slots.push_back({0x3B, &r.texa.bits});
    slots.push_back({0x3D, &r.fogcol.bits});
    slots.push_back({0x44, &r.dimx.bits});
    slots.push_back({0x45, &r.dthe.bits});
    slots.push_back({0x46, &r.colclamp.bits});
    slots.push_back({0x49, &r.pabe.bits});
    slots.push_back({0x50, &r.bitbltbuf.bits});
    slots.push_back({0x51, &r.trxpos.bits});
    slots.push_back({0x52, &r.trxreg.bits});
    slots.push_back({0x53, &r.trxdir.bits});
    slots.push_back({0x22, &r.scanmsk.bits});
    return slots;
}

// A PortStateIo that appends to a buffer (save_field writes through one), or reads from one
struct BufferIo {
    PortStateIo io;
    std::vector<uint8_t> *out;
    const uint8_t *in;
    size_t in_size, at;
    bool ok;
};

void buffer_io_bytes(PortStateIo *io, void *data, size_t size) {
    BufferIo *b = reinterpret_cast<BufferIo *>(io);
    if (b->out) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        b->out->insert(b->out->end(), p, p + size);
        return;
    }
    if (b->at + size > b->in_size) {
        b->ok = false;
        if (data)
            memset(data, 0, size);
        return;
    }
    if (data)
        memcpy(data, b->in + b->at, size);
    b->at += size;
}
} // namespace

extern "C" const void *port_gs_backend_vram(void) {
    gs->flush();
    return gs->map_vram_read(0, vram_bytes);
}

// A rectangle of a buffer as the GS has it, read back through the GS itself (a local-to-host
// transfer, as a game would), into RGBA (the 32-bit formats as they are; the 16-bit ones widened, their
// alpha bit 0x80). paraLLEl-GS's VRAM mirror (below) can be stale for pages draws wrote. The transfer
// registers are put back. For comparing the renderers.
extern "C" void port_gs_backend_read_rect(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                                          uint32_t h, uint32_t *rgba) {
    RegisterState saved = gs->get_register_state();
    bool half = psm == 0x02 || psm == 0x0A;
    uint64_t packet[2 * 5];
    packet[0] = 4 | (1ull << 15) | (1ull << 60); // GIF tag: NLOOP 4, EOP, PACKED, one register: A+D
    packet[1] = 0xE;
    packet[2] = uint64_t(bp) | (uint64_t(bw) << 16) | (uint64_t(psm) << 24);
    packet[3] = 0x50;
    packet[4] = uint64_t(x) | (uint64_t(y) << 16);
    packet[5] = 0x51;
    packet[6] = w | (uint64_t(h) << 32);
    packet[7] = 0x52;
    packet[8] = 1; // TRXDIR: local to host
    packet[9] = 0x53;
    gs->gif_transfer(2, packet, 5 * 16);
    size_t bpp = half ? 2 : psm == 0x01 ? 3 : 4;
    size_t bytes = size_t(w) * h * bpp;
    std::vector<uint8_t> data((bytes + 15) & ~size_t(15));
    gs->read_transfer_fifo(data.data(), uint32_t(data.size() / 16));
    for (size_t i = 0; i < size_t(w) * h; i++) {
        if (half) {
            uint32_t v = data[i * 2] | (data[i * 2 + 1] << 8);
            rgba[i] = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) |
                      ((v & 0x8000) ? 0x80000000u : 0);
        } else if (bpp == 3) {
            rgba[i] = data[i * 3] | (data[i * 3 + 1] << 8) | (uint32_t(data[i * 3 + 2]) << 16);
        } else {
            memcpy(&rgba[i], &data[i * 4], 4);
        }
    }
    RegisterState &now = gs->get_register_state();
    now.bitbltbuf = saved.bitbltbuf;
    now.trxpos = saved.trxpos;
    now.trxreg = saved.trxreg;
    now.trxdir = saved.trxdir;
}


extern "C" void port_gs_backend_capture(void (*emit)(uint32_t type, const void *data, size_t size)) {
    emit(KF4GS_VRAM, port_gs_backend_vram(), vram_bytes);

    RegisterState registers = gs->get_register_state();
    std::vector<Kf4GsReg> regs;
    for (const RegSlot &slot : register_slots(registers))
        regs.push_back({slot.address, 0, *slot.bits});
    uint32_t q_bits;
    memcpy(&q_bits, &registers.internal_q, 4);
    regs.push_back({KF4GS_REG_INTERNAL_Q, 0, q_bits});
    regs.push_back({KF4GS_REG_CACHED_CBP0, 0, registers.cached_cbp[0]});
    regs.push_back({KF4GS_REG_CACHED_CBP1, 0, registers.cached_cbp[1]});
    emit(KF4GS_GS_REGS, regs.data(), regs.size() * sizeof(Kf4GsReg));

    const PrivRegisterState &priv = gs->get_priv_register_state();
    std::vector<uint64_t> privileged(0x400);
    memcpy(privileged.data(), priv.qwords_lo, 0x200 * 8);
    memcpy(privileged.data() + 0x200, priv.qwords_hi, 0x200 * 8);
    emit(KF4GS_GS_PRIV, privileged.data(), privileged.size() * 8);

    std::vector<uint8_t> history_bytes;
    BufferIo b = {{buffer_io_bytes, 0}, &history_bytes, nullptr, 0, 0, true};
    ImageHandle *fields = history();
    uint32_t head[2] = {gs_scanmsk_timeout(*gs), uint32_t(history_fields)};
    buffer_io_bytes(&b.io, head, sizeof(head));
    for (int i = 0; i < history_fields; i++) {
        FieldHeader h = field_header(fields, i);
        Kf4GsField f = {h.same_as, h.width, h.height, h.present};
        buffer_io_bytes(&b.io, &f, sizeof(f));
        if (h.present && h.same_as < 0)
            save_field(&b.io, *fields[i]);
    }
    emit(KF4GS_GS_HISTORY, history_bytes.data(), history_bytes.size());
}

extern "C" int port_gs_backend_restore(uint32_t type, const void *data, size_t size) {
    switch (type) {
    case KF4GS_VRAM:
        if (size != vram_bytes)
            return 0;
        gs->reset_context_state();
        memcpy(gs->map_vram_write(0, vram_bytes), data, vram_bytes);
        gs->end_vram_write(0, vram_bytes);
        return 1;
    case KF4GS_GS_REGS: {
        RegisterState &registers = gs->get_register_state();
        std::vector<RegSlot> slots = register_slots(registers);
        const Kf4GsReg *regs = static_cast<const Kf4GsReg *>(data);
        for (size_t n = 0; n < size / sizeof(Kf4GsReg); n++) {
            uint32_t low = uint32_t(regs[n].value);
            switch (regs[n].address) {
            case KF4GS_REG_INTERNAL_Q: memcpy(&registers.internal_q, &low, 4); continue;
            // The GS's CLUT buffer is on the chip, not in VRAM, and is not in a capture: the CBP it
            // last loaded is forgotten, so that the first TEX0 that compares (CLD 4, 5) loads the
            // CLUT again from VRAM, which is what the buffer holds unless VRAM there changed since.
            case KF4GS_REG_CACHED_CBP0: registers.cached_cbp[0] = UINT32_MAX; continue;
            case KF4GS_REG_CACHED_CBP1: registers.cached_cbp[1] = UINT32_MAX; continue;
            }
            for (const RegSlot &slot : slots)
                if (slot.address == regs[n].address)
                    *slot.bits = regs[n].value;
        }
        gs->clobber_register_state();
        gs->flush();
        return 1;
    }
    case KF4GS_GS_PRIV: {
        if (size != 0x400 * 8)
            return 0;
        PrivRegisterState &priv = gs->get_priv_register_state();
        memcpy(priv.qwords_lo, data, 0x200 * 8);
        memcpy(priv.qwords_hi, static_cast<const uint64_t *>(data) + 0x200, 0x200 * 8);
        gs->clobber_register_state();
        gs->flush();
        return 1;
    }
    case KF4GS_GS_HISTORY: {
        BufferIo b = {{buffer_io_bytes, 1}, nullptr, static_cast<const uint8_t *>(data), size, 0, true};
        uint32_t head[2];
        buffer_io_bytes(&b.io, head, sizeof(head));
        gs_scanmsk_timeout(*gs) = head[0];
        ImageHandle *fields = history();
        for (int i = 0; i < history_fields && i < int(head[1]) && b.ok; i++) {
            Kf4GsField f;
            buffer_io_bytes(&b.io, &f, sizeof(f));
            if (!f.present)
                fields[i].reset();
            else if (f.same_as >= 0 && f.same_as < i)
                fields[i] = fields[f.same_as];
            else
                fields[i] = load_field(&b.io, f.width, f.height);
        }
        return b.ok;
    }
    }
    return 0;
}
